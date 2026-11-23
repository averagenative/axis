/*
 * axis-tv-absolute.c - an anime series' absolute episode numbers: the state
 * they are in, and the other app's numbers beside TheTVDB's
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * add-anime-support, slice A1: the state of a series' numbers and the
 * comparison with Sonarr's, pure (they read what they are given and say what
 * that is), for the series' page, the migration's preview and its report.
 * Slice A2 adds, at the end, what the matcher of a name numbered absolutely
 * is given of one series (axis_tv_anime_init(), which reads the series' other
 * names for an anime series and nothing for any other); the matcher itself
 * is axis_tv_match_release_anime() in axis-tv-state.c.
 *
 * The rule the later slices rest on is stated once, here: a series' absolute
 * numbers are usable only when the series follows TheTVDB's aired order, each
 * number is on exactly one live episode of seasons 1 and up, and the numbers
 * rise with season and episode.  One number on two episodes, or one number
 * out of order, makes the whole series' numbers not usable.  A special is
 * never counted.
 */

#include "modules/tv/axis-tv-private.h"

#include <json-glib/json-glib.h>
#include <string.h>

/* TheTVDB's aired order: the one numbering that carries absolute numbers. */
#define ABSOLUTE_SOURCE "tvdb"

static gint
compare_place(
	gconstpointer a,
	gconstpointer b
){
	const AxisTvEpisode *x = *(const AxisTvEpisode *const *)a;
	const AxisTvEpisode *y = *(const AxisTvEpisode *const *)b;

	if (x->season != y->season)
		return x->season < y->season ? -1 : 1;
	if (x->number != y->number)
		return x->number < y->number ? -1 : 1;
	return x->id < y->id ? -1 : x->id > y->id ? 1 : 0;
}

void
axis_tv_absolute_state(
	const AxisTvSeries *series,
	const GPtrArray    *episodes,
	AxisTvAbsolute     *absolute
){
	g_autoptr(GPtrArray) live = g_ptr_array_new();
	g_autoptr(GHashTable) seen = g_hash_table_new(g_direct_hash, g_direct_equal);
	const AxisTvEpisode *before = NULL;
	gboolean plain = TRUE;
	guint i;

	memset(absolute, 0, sizeof(*absolute));
	absolute->state = AXIS_TV_ABSOLUTE_NO_SOURCE;
	for (i = 0; episodes != NULL && i < episodes->len; i++) {
		const AxisTvEpisode *e = g_ptr_array_index(episodes, i);

		/* A special is never counted, whatever TheTVDB gives it; nor an
		 * episode the numbering no longer lists. */
		if (e->gone || e->season < 1)
			continue;
		g_ptr_array_add(live, (gpointer)e);
	}
	g_ptr_array_sort(live, compare_place);
	absolute->n_episodes = live->len;
	if (series == NULL || g_strcmp0(series->episode_source, ABSOLUTE_SOURCE) != 0)
		return;
	for (i = 0; i < live->len; i++) {
		const AxisTvEpisode *e = g_ptr_array_index(live, i);
		const AxisTvEpisode *same;

		if (e->absolute <= 0)
			continue;
		absolute->n_numbered++;
		if (absolute->state == AXIS_TV_ABSOLUTE_NOT_USABLE)
			continue;
		same = g_hash_table_lookup(seen, GINT_TO_POINTER(e->absolute));
		if (same != NULL || (before != NULL && e->absolute <= before->absolute)) {
			/* The first fault is the one named: a number on two episodes,
			 * else a number that is not above the one before it. */
			const AxisTvEpisode *first = same != NULL ? same : before;

			absolute->state = AXIS_TV_ABSOLUTE_NOT_USABLE;
			absolute->duplicate = same != NULL;
			absolute->fault_season[0] = first->season;
			absolute->fault_number[0] = first->number;
			absolute->fault_absolute[0] = first->absolute;
			absolute->fault_season[1] = e->season;
			absolute->fault_number[1] = e->number;
			absolute->fault_absolute[1] = e->absolute;
			continue;
		}
		g_hash_table_insert(seen, GINT_TO_POINTER(e->absolute), (gpointer)e);
		before = e;
		/* Beside a plain count of the episodes in aired order: information
		 * for a person, never a number Axis uses. */
		if (plain && (guint)e->absolute != i + 1) {
			plain = FALSE;
			absolute->off_season = e->season;
			absolute->off_number = e->number;
			absolute->off_absolute = e->absolute;
			absolute->off_count = i + 1;
		}
	}
	if (absolute->state == AXIS_TV_ABSOLUTE_NOT_USABLE)
		return;
	if (absolute->n_numbered == 0) {
		absolute->state = AXIS_TV_ABSOLUTE_NONE_GIVEN;
		return;
	}
	absolute->state = AXIS_TV_ABSOLUTE_USABLE;
	absolute->plain_count = plain;
}

gchar *
axis_tv_absolute_sentence(
	const AxisTvSeries   *series,
	const AxisTvAbsolute *absolute,
	gboolean              tvdb_key
){
	g_autofree gchar *first = NULL;
	g_autofree gchar *second = NULL;

	switch (absolute->state) {
	case AXIS_TV_ABSOLUTE_NONE_GIVEN:
		return g_strdup("TheTVDB gives no absolute numbers for this series.");
	case AXIS_TV_ABSOLUTE_USABLE:
		if (absolute->plain_count)
			return g_strdup_printf("TheTVDB gives absolute numbers for %u of %u %s. They equal a plain count of the "
				"episodes in aired order.", absolute->n_numbered, absolute->n_episodes,
				absolute->n_episodes == 1 ? "episode" : "episodes");
		first = axis_tv_episode_label(absolute->off_season, absolute->off_number);
		return g_strdup_printf("TheTVDB gives absolute numbers for %u of %u %s. From %s on they differ from a plain "
			"count of the episodes in aired order (%s is %d at TheTVDB and %u in the count). That is not an error by "
			"itself: TheTVDB may count an episode that a plain count does not. It is the place to look.",
			absolute->n_numbered, absolute->n_episodes, absolute->n_episodes == 1 ? "episode" : "episodes", first, first,
			absolute->off_absolute, absolute->off_count);
	case AXIS_TV_ABSOLUTE_NOT_USABLE:
		first = axis_tv_episode_label(absolute->fault_season[0], absolute->fault_number[0]);
		second = axis_tv_episode_label(absolute->fault_season[1], absolute->fault_number[1]);
		if (absolute->duplicate)
			return g_strdup_printf("TheTVDB gives two episodes of this series the same absolute number (%s and %s are "
				"both %d). Axis does not use this series' absolute numbers. Releases numbered absolutely are not matched "
				"until TheTVDB's data is corrected.", first, second, absolute->fault_absolute[1]);
		return g_strdup_printf("TheTVDB's absolute numbers for this series are not in order (%s is %d and %s is %d). "
			"Axis does not use them. Releases numbered absolutely are not matched until TheTVDB's data is corrected.",
			first, absolute->fault_absolute[0], second, absolute->fault_absolute[1]);
	default:
		break;
	}
	/* (Whatever it follows that is not TheTVDB's aired order: TMDb gives no
	 * absolute numbers, and TheTVDB's other orders are not built.) */
	return g_strdup_printf("Absolute numbers come from TheTVDB's aired order, and this series is numbered as %s numbers "
		"it, which has none. %s", axis_tv_source_label(series != NULL ? series->episode_source : NULL), tvdb_key ?
		"To have absolute numbers for it, change its numbering to TheTVDB's aired order." :
		"Set a TheTVDB key on the Movie and TV details page first: a series can follow TheTVDB's aired order only while "
		"Axis has one.");
}

/* ── The other app's numbers beside TheTVDB's ───────────────────── */

void
axis_tv_absolute_compare_init(AxisTvAbsoluteCompare *compare)
{
	memset(compare, 0, sizeof(*compare));
	compare->differs = g_array_new(FALSE, TRUE, sizeof(AxisTvAbsoluteDiffer));
}

void
axis_tv_absolute_compare_clear(AxisTvAbsoluteCompare *compare)
{
	g_clear_pointer(&compare->differs, g_array_unref);
}

void
axis_tv_absolute_compare_add(
	AxisTvAbsoluteCompare *compare,
	gint                   season,
	gint                   number,
	gint                   theirs,
	gint                   tvdb
){
	if (theirs <= 0 && tvdb <= 0)
		return;
	if (theirs <= 0 || tvdb <= 0) {
		compare->one_side++;
		return;
	}
	if (theirs == tvdb) {
		compare->agree++;
		return;
	}
	compare->differ++;
	if (compare->differs->len < AXIS_TV_ABSOLUTE_LISTED) {
		AxisTvAbsoluteDiffer differ;

		differ.season = season;
		differ.number = number;
		differ.theirs = theirs;
		differ.tvdb = tvdb;
		g_array_append_val(compare->differs, differ);
	}
}

gchar *
axis_tv_absolute_compare_json(const AxisTvAbsoluteCompare *compare)
{
	GString *json = g_string_new(NULL);
	guint i;

	g_string_append_printf(json, "{\"agree\":%u,\"differ\":%u,\"one_side\":%u,\"list\":[", compare->agree,
		compare->differ, compare->one_side);
	for (i = 0; i < compare->differs->len; i++) {
		const AxisTvAbsoluteDiffer *d = &g_array_index(compare->differs, AxisTvAbsoluteDiffer, i);

		g_string_append_printf(json, "%s[%d,%d,%d,%d]", i > 0 ? "," : "", d->season, d->number, d->theirs, d->tvdb);
	}
	g_string_append(json, "]}");
	return g_string_free(json, FALSE);
}

/* A count from 0 to a million of them; %FALSE for anything else. */
static gboolean
member_count(
	JsonObject  *object,
	const gchar *name,
	guint       *value
){
	JsonNode *node = json_object_get_member(object, name);

	if (node == NULL || !JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_INT64 ||
	    json_node_get_int(node) < 0 || json_node_get_int(node) > 100000000)
		return FALSE;
	*value = (guint)json_node_get_int(node);
	return TRUE;
}

static gboolean
element_int(
	JsonArray *array,
	guint      at,
	gint      *value
){
	JsonNode *node = json_array_get_element(array, at);

	if (node == NULL || !JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_INT64 ||
	    json_node_get_int(node) < 0 || json_node_get_int(node) > 100000000)
		return FALSE;
	*value = (gint)json_node_get_int(node);
	return TRUE;
}

gboolean
axis_tv_absolute_compare_parse(
	const gchar           *json,
	AxisTvAbsoluteCompare *compare
){
	g_autoptr(JsonParser) parser = json_parser_new();
	JsonObject *object;
	JsonNode *list;
	guint i;

	axis_tv_absolute_compare_init(compare);
	if (json == NULL || !json_parser_load_from_data(parser, json, -1, NULL) || json_parser_get_root(parser) == NULL ||
	    !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)))
		return FALSE;
	object = json_node_get_object(json_parser_get_root(parser));
	if (!member_count(object, "agree", &compare->agree) || !member_count(object, "differ", &compare->differ) ||
	    !member_count(object, "one_side", &compare->one_side)) {
		compare->agree = compare->differ = compare->one_side = 0;
		return FALSE;
	}
	list = json_object_get_member(object, "list");
	if (list == NULL || !JSON_NODE_HOLDS_ARRAY(list))
		return TRUE;
	for (i = 0; i < json_array_get_length(json_node_get_array(list)) && i < AXIS_TV_ABSOLUTE_LISTED; i++) {
		JsonNode *entry = json_array_get_element(json_node_get_array(list), i);
		AxisTvAbsoluteDiffer differ;

		if (entry == NULL || !JSON_NODE_HOLDS_ARRAY(entry) || json_array_get_length(json_node_get_array(entry)) != 4 ||
		    !element_int(json_node_get_array(entry), 0, &differ.season) ||
		    !element_int(json_node_get_array(entry), 1, &differ.number) ||
		    !element_int(json_node_get_array(entry), 2, &differ.theirs) ||
		    !element_int(json_node_get_array(entry), 3, &differ.tvdb))
			continue;
		g_array_append_val(compare->differs, differ);
	}
	return TRUE;
}

static gchar *
differ_text(const AxisTvAbsoluteDiffer *differ)
{
	g_autofree gchar *label = axis_tv_episode_label(differ->season, differ->number);

	return g_strdup_printf("%s (Sonarr %d, TheTVDB %d)", label, differ->theirs, differ->tvdb);
}

gchar *
axis_tv_absolute_compare_sentence(const AxisTvAbsoluteCompare *compare)
{
	guint total = compare->agree + compare->differ + compare->one_side;
	GString *text;

	if (total == 0)
		return g_strdup("Absolute numbers: neither Sonarr nor TheTVDB gives one for any episode of it, so there was "
			"nothing to compare.");
	text = g_string_new(NULL);
	if (compare->differ == 0) {
		g_string_append_printf(text, "Absolute numbers: Sonarr and TheTVDB agree for %u of %u %s.", compare->agree, total,
			total == 1 ? "episode" : "episodes");
	} else {
		g_autofree gchar *first = compare->differs->len > 0 ?
			differ_text(&g_array_index(compare->differs, AxisTvAbsoluteDiffer, 0)) : NULL;

		g_string_append_printf(text, "Absolute numbers: Sonarr and TheTVDB differ for %u of %u %s", compare->differ,
			total, total == 1 ? "episode" : "episodes");
		if (first != NULL)
			g_string_append_printf(text, ", the first %s", first);
		g_string_append_printf(text, ", and agree for %u.", compare->agree);
	}
	if (compare->one_side > 0)
		g_string_append_printf(text, " For %u only one of the two gives a number.", compare->one_side);
	if (compare->differ > 0)
		g_string_append(text, " Nothing was migrated differently because of it: every file is on the episode Sonarr "
			"had it on.");
	return g_string_free(text, FALSE);
}

gchar *
axis_tv_absolute_compare_list(const AxisTvAbsoluteCompare *compare)
{
	GString *text;
	guint i;

	if (compare->differs == NULL || compare->differs->len == 0)
		return NULL;
	text = g_string_new(NULL);
	for (i = 0; i < compare->differs->len; i++) {
		g_autofree gchar *one = differ_text(&g_array_index(compare->differs, AxisTvAbsoluteDiffer, i));

		g_string_append_printf(text, "%s%s", i > 0 ? "; " : "", one);
	}
	if (compare->differ > compare->differs->len)
		g_string_append_printf(text, "; and %u more", compare->differ - compare->differs->len);
	return g_string_free(text, FALSE);
}

/* ── What the matcher is given (slice A2) ───────────────────────── */

static void
anime_add_name(
	AxisTvAnime *anime,
	const gchar *name
){
	if (name != NULL && name[0] != '\0')
		g_ptr_array_add(anime->names, g_strdup(name));
}

gboolean
axis_tv_anime_init(
	AxisTvAnime         *anime,
	AxisDbConn          *conn,
	const AxisTvSeries  *series,
	const GPtrArray     *episodes,
	const gchar         *today,
	GError             **error
){
	AxisTvAbsolute absolute;
	guint i;

	memset(anime, 0, sizeof(*anime));
	/* Not marked as anime: nothing is read, and every name is matched as it
	 * is for any series. */
	if (series == NULL || !series->anime)
		return TRUE;
	anime->read.anime = TRUE;
	axis_tv_absolute_state(series, episodes, &absolute);
	anime->read.state = absolute.state;
	g_strlcpy(anime->today, today != NULL ? today : "", sizeof(anime->today));
	anime->read.today = anime->today[0] != '\0' ? anime->today : NULL;
	anime->absolute = g_new0(gint, MAX(episodes != NULL ? episodes->len : 0, 1));
	for (i = 0; episodes != NULL && i < episodes->len; i++) {
		const AxisTvEpisode *e = g_ptr_array_index(episodes, i);

		/* A number only under the numbering that carries them. */
		anime->absolute[i] = absolute.state == AXIS_TV_ABSOLUTE_USABLE && !e->gone && e->absolute > 0 ? e->absolute : 0;
	}
	anime->read.absolute = anime->absolute;
	anime->names = g_ptr_array_new_with_free_func(g_free);
	anime_add_name(anime, series->title);
	anime_add_name(anime, series->original_title);
	/* These are the series' own: an absolute number counts its episodes
	 * under them, and not under a longer name among the others. */
	anime->read.n_own = anime->names->len;
	anime->read.year = series->year;
	if (conn != NULL) {
		g_autoptr(AxisDbResult) alts = NULL;
		g_autofree gchar *title = g_strdup_printf("%" G_GINT64_FORMAT, series->title_id);
		const gchar *params[1];

		params[0] = title;
		if (!axis_db_conn_exec(conn, "SELECT title FROM meta_alt_title WHERE title_id = $1::bigint ORDER BY title"
		        " LIMIT 100", 1, params, &alts, error)) {
			axis_tv_anime_clear(anime);
			return FALSE;
		}
		for (i = 0; i < axis_db_result_get_n_rows(alts); i++)
			anime_add_name(anime, axis_db_result_get_text(alts, i, 0));
	}
	anime->read.titles = (const gchar *const *)anime->names->pdata;
	anime->read.n_titles = anime->names->len;
	return TRUE;
}

void
axis_tv_anime_clear(AxisTvAnime *anime)
{
	if (anime == NULL)
		return;
	g_free(anime->absolute);
	g_clear_pointer(&anime->names, g_ptr_array_unref);
	memset(anime, 0, sizeof(*anime));
}

void
axis_tv_name_episode_anime(
	const gchar        *path,
	AxisTvAnime        *anime,
	const AxisTvKnown  *known,
	guint               n,
	gint               *season,
	gint               *number,
	gint               *absolute
){
	g_autofree gchar *base = NULL;
	g_autoptr(AxisRelease) release = NULL;
	AxisTvHolds holds;

	if (absolute != NULL)
		*absolute = 0;
	axis_tv_name_episode(path, season, number);
	if (*season >= 0 || anime == NULL || !anime->read.anime || path == NULL)
		return;
	base = g_path_get_basename(path);
	release = axis_release_parse(base);
	anime->read.name = base;
	axis_tv_match_release_anime(release, known, n, &anime->read, &holds);
	anime->read.name = NULL;
	if (holds.ok && holds.by_absolute && !holds.pack && holds.indices->len == 1) {
		guint at = g_array_index(holds.indices, guint, 0);

		*season = known[at].season;
		*number = known[at].number;
		if (absolute != NULL)
			*absolute = anime->absolute[at];
	}
	axis_tv_holds_clear(&holds);
}

gboolean
axis_tv_title_hints_anime(
	const GPtrArray *genres,
	const gchar     *original_language
){
	guint i;

	if (original_language == NULL || g_ascii_strcasecmp(original_language, "ja") != 0)
		return FALSE;
	for (i = 0; genres != NULL && i < genres->len; i++) {
		const gchar *genre = g_ptr_array_index(genres, i);

		if (genre != NULL && (g_ascii_strcasecmp(genre, "Animation") == 0 || g_ascii_strcasecmp(genre, "Anime") == 0))
			return TRUE;
	}
	return FALSE;
}

/* ── What a search asks for (slice A3) ──────────────────────────── */

gboolean
axis_tv_name_is_latin(const gchar *name)
{
	const gchar *p;
	gboolean letter = FALSE;

	if (name == NULL || !g_utf8_validate(name, -1, NULL))
		return FALSE;
	for (p = name; *p != '\0'; p = g_utf8_next_char(p)) {
		gunichar c = g_utf8_get_char(p);

		if (!g_unichar_isalpha(c))
			continue;
		if (g_unichar_get_script(c) != G_UNICODE_SCRIPT_LATIN)
			return FALSE;
		letter = TRUE;
	}
	return letter;
}

/* Whether @name can be asked by beside the title whose key is @main: Latin
 * letters, a key of its own, and not a name that holds the title's (a part's
 * name, under which no absolute number is read). */
static gboolean
second_name_fits(
	const gchar *name,
	const gchar *main
){
	g_autofree gchar *key = NULL;

	if (name == NULL || name[0] == '\0' || !axis_tv_name_is_latin(name))
		return FALSE;
	key = axis_release_title_key(name);
	if (key == NULL || key[0] == '\0')
		return FALSE;
	return main == NULL || main[0] == '\0' || strstr(key, main) == NULL;
}

const gchar *
axis_tv_anime_second_name(const AxisMetaTitle *record)
{
	g_autofree gchar *main = NULL;
	const gchar *best = NULL;
	guint i;

	if (record == NULL)
		return NULL;
	main = record->title != NULL ? axis_release_title_key(record->title) : NULL;
	/* (An original title that is the title again says nothing new: the other
	 * names are looked at then, as when it is not in Latin letters.) */
	if (second_name_fits(record->original_title, main))
		return record->original_title;
	for (i = 0; record->alt_titles != NULL && i < record->alt_titles->len; i++) {
		const AxisMetaAltTitle *alt = g_ptr_array_index(record->alt_titles, i);

		if (alt == NULL || alt->title == NULL)
			continue;
		if (!(alt->language != NULL && g_ascii_strcasecmp(alt->language, "ja") == 0) &&
		    !(alt->country != NULL && g_ascii_strcasecmp(alt->country, "JP") == 0))
			continue;
		if (!second_name_fits(alt->title, main))
			continue;
		if (best == NULL || strcmp(alt->title, best) < 0)
			best = alt->title;
	}
	return best;
}

guint
axis_tv_absolute_digits(
	const gint *absolute,
	guint       n
){
	guint i;

	for (i = 0; absolute != NULL && i < n; i++) {
		if (absolute[i] >= 100)
			return 3;
	}
	return 2;
}

gchar *
axis_tv_absolute_words(
	const gchar *name,
	gint         absolute,
	guint        digits
){
	g_autofree gchar *clean = name != NULL ? g_strstrip(g_strdup(name)) : NULL;

	if (clean == NULL || clean[0] == '\0' || absolute < 1 || absolute > 9999)
		return NULL;
	return g_strdup_printf("%s %0*d", clean, digits >= 3 ? 3 : 2, absolute);
}
