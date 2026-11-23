/*
 * axis-identify-rules.c - names, units, the decision and a file's quality
 * (see the header)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "identify/axis-identify-rules.h"

#include <string.h>

static const gchar *const confidence_names[] = { "sure", "likely", "unknown" };
static const gchar *const confidence_labels[] = { "Sure", "Likely", "Unknown" };

const gchar *
axis_identify_confidence_name(AxisIdentifyConfidence confidence)
{
	return (guint)confidence < G_N_ELEMENTS(confidence_names) ? confidence_names[confidence] : "unknown";
}

gboolean
axis_identify_confidence_from_name(
	const gchar            *name,
	AxisIdentifyConfidence *confidence
){
	guint i;

	for (i = 0; name != NULL && i < G_N_ELEMENTS(confidence_names); i++) {
		if (strcmp(name, confidence_names[i]) == 0) {
			*confidence = (AxisIdentifyConfidence)i;
			return TRUE;
		}
	}
	return FALSE;
}

const gchar *
axis_identify_confidence_label(AxisIdentifyConfidence confidence)
{
	return (guint)confidence < G_N_ELEMENTS(confidence_labels) ? confidence_labels[confidence] : "Unknown";
}

/* ── Names ──────────────────────────────────────────────────────── */

/*
 * A key that is a word names give to anything ("movie", "CD1", "01"), in a
 * name that @release read from it.  A name that is exactly a folder's
 * "<something> (<year>)" names its title, whatever that is made of ("412
 * (2011)", "CD1 (2007)", letters that make a Roman numeral, whose key is a
 * number); a short number with a year beside it is a title too
 * ("412.2011.1080p...").  A number alone ("01.mkv") is not.
 */
static gboolean
generic_key(
	const gchar       *key,
	const AxisRelease *release
){
	static const gchar *const words[] = {
		"movie", "film", "video", "feature", "main", "sample", "trailer", "title", "untitled", "track", "vts", "cd",
		"disc", "disk", "part", "pt", "dvd", "bluray", "bdmv", "stream", "file", "new", NULL
	};
	gsize n = strlen(key);
	gsize digits = 0;
	guint i;

	if (release->folder_form)
		return FALSE;
	while (digits < n && g_ascii_isdigit(key[n - 1 - digits]))
		digits++;
	if (digits == n)
		return n <= 3 && release->year == 0;
	if (digits > 3)
		return FALSE;
	for (i = 0; words[i] != NULL; i++) {
		if (strlen(words[i]) == n - digits && strncmp(key, words[i], n - digits) == 0)
			return TRUE;
	}
	return FALSE;
}

AxisIdentifyName *
axis_identify_name_read(
	const gchar *name,
	gint         latest_year
){
	AxisIdentifyName *read = g_new0(AxisIdentifyName, 1);
	g_autofree gchar *stripped = NULL;

	read->shown = g_strdup(name != NULL ? name : "");
	stripped = axis_identify_name_ids(read->shown, -1, &read->ids);
	read->release = latest_year > 0 ? axis_release_parse_full(stripped, -1, latest_year) : axis_release_parse(stripped);
	if (read->release->title != NULL && (read->release->stop & AXIS_RELEASE_STOP_OBFUSCATED) == 0) {
		gchar *key = axis_release_title_key(read->release->title);

		if (key != NULL && key[0] != '\0' && !generic_key(key, read->release))
			read->key = key;
		else
			g_free(key);
	}
	read->year = read->key != NULL ? read->release->year : 0;
	return read;
}

void
axis_identify_name_free(AxisIdentifyName *name)
{
	if (name == NULL)
		return;
	g_free(name->shown);
	axis_release_free(name->release);
	g_free(name->key);
	g_free(name);
}

gchar *
axis_identify_name_describe(const AxisIdentifyName *name)
{
	if (name == NULL || name->key == NULL)
		return g_strdup("nothing that reads as a title");
	if (name->year > 0)
		return g_strdup_printf("%s (%d)", name->release->title, name->year);
	return g_strdup_printf("%s, with no year", name->release->title);
}

/* ── Units of a folder ──────────────────────────────────────────── */

void
axis_identify_group_free(AxisIdentifyGroup *group)
{
	if (group == NULL)
		return;
	g_array_unref(group->members);
	g_free(group);
}

typedef struct
{
	const gchar *key;
	gint         year;
	GArray      *members;
	gboolean     gone;
} Cluster;

static AxisIdentifyGroup *
group_new(gboolean folder)
{
	AxisIdentifyGroup *group = g_new0(AxisIdentifyGroup, 1);

	group->folder = folder;
	group->members = g_array_new(FALSE, FALSE, sizeof(guint));
	return group;
}

static gint
compare_index(
	gconstpointer a,
	gconstpointer b
){
	guint x = *(const guint *)a;
	guint y = *(const guint *)b;

	return x < y ? -1 : x > y;
}

static void
merge_into(
	Cluster *to,
	Cluster *from
){
	g_array_append_vals(to->members, from->members->data, from->members->len);
	if (to->year == 0)
		to->year = from->year;
	from->gone = TRUE;
}

GPtrArray *
axis_identify_group(
	const AxisIdentifyName        *folder,
	const AxisIdentifyName *const *files,
	guint                          n
){
	GPtrArray *groups = g_ptr_array_new_with_free_func((GDestroyNotify)axis_identify_group_free);
	g_autofree Cluster *clusters = NULL;
	g_autoptr(GArray) keyless = NULL;
	AxisIdentifyGroup *group;
	guint n_clusters = 0;
	guint live = 0;
	guint compatible = 0;
	gint folder_cluster = -1;
	guint i;
	guint j;

	if (n == 0)
		return groups;
	if (folder == NULL) {
		for (i = 0; i < n; i++) {
			group = group_new(FALSE);
			g_array_append_val(group->members, i);
			g_ptr_array_add(groups, group);
		}
		return groups;
	}
	if (n == 1) {
		group = group_new(TRUE);
		i = 0;
		g_array_append_val(group->members, i);
		g_ptr_array_add(groups, group);
		return groups;
	}

	/* By title and year. */
	clusters = g_new0(Cluster, n);
	keyless = g_array_new(FALSE, FALSE, sizeof(guint));
	for (i = 0; i < n; i++) {
		const AxisIdentifyName *file = files[i];

		if (file == NULL || file->key == NULL) {
			g_array_append_val(keyless, i);
			continue;
		}
		for (j = 0; j < n_clusters; j++) {
			if (strcmp(clusters[j].key, file->key) == 0 && clusters[j].year == file->year)
				break;
		}
		if (j == n_clusters) {
			clusters[j].key = file->key;
			clusters[j].year = file->year;
			clusters[j].members = g_array_new(FALSE, FALSE, sizeof(guint));
			n_clusters++;
		}
		g_array_append_val(clusters[j].members, i);
	}
	/* Years one apart are one film (release dates differ by country). */
	for (i = 0; i < n_clusters; i++) {
		for (j = i + 1; j < n_clusters; j++) {
			if (!clusters[i].gone && !clusters[j].gone && strcmp(clusters[i].key, clusters[j].key) == 0 &&
			    clusters[i].year > 0 && clusters[j].year > 0 && ABS(clusters[i].year - clusters[j].year) <= 1)
				merge_into(&clusters[i], &clusters[j]);
		}
	}
	/* No year: the one group of that title, when there is just one and the
	 * folder's own name gives that title and its year.  Otherwise a video
	 * named without a year beside one named with it may be another film of
	 * the name (a remake), and nothing says it is not: they stay apart. */
	for (i = 0; i < n_clusters; i++) {
		gint only = -1;
		guint same = 0;

		if (clusters[i].gone || clusters[i].year != 0)
			continue;
		for (j = 0; j < n_clusters; j++) {
			if (j != i && !clusters[j].gone && clusters[j].year != 0 && strcmp(clusters[i].key, clusters[j].key) == 0) {
				only = (gint)j;
				same++;
			}
		}
		if (same == 1 && folder->key != NULL && strcmp(folder->key, clusters[i].key) == 0 && folder->year > 0 &&
		    ABS(folder->year - clusters[only].year) <= 1)
			merge_into(&clusters[only], &clusters[i]);
	}
	for (i = 0; i < n_clusters; i++) {
		if (clusters[i].gone)
			continue;
		live++;
		if (folder->key != NULL && strcmp(clusters[i].key, folder->key) == 0 &&
		    (folder->year == 0 || clusters[i].year == 0 || ABS(clusters[i].year - folder->year) <= 1)) {
			compatible++;
			folder_cluster = (gint)i;
		}
	}

	if (live <= 1) {
		/* One title (or none): the folder's unit, every video. */
		group = group_new(TRUE);
		for (i = 0; i < n; i++)
			g_array_append_val(group->members, i);
		g_ptr_array_add(groups, group);
	} else {
		/* Several titles: the folder keeps its own title's and the nameless. */
		group = group_new(TRUE);
		g_array_append_vals(group->members, keyless->data, keyless->len);
		if (compatible == 1)
			g_array_append_vals(group->members, clusters[folder_cluster].members->data,
				clusters[folder_cluster].members->len);
		if (group->members->len > 0) {
			g_array_sort(group->members, compare_index);
			g_ptr_array_add(groups, group);
		} else {
			axis_identify_group_free(group);
		}
		for (i = 0; i < n_clusters; i++) {
			if (clusters[i].gone || (compatible == 1 && (gint)i == folder_cluster))
				continue;
			group = group_new(FALSE);
			g_array_append_vals(group->members, clusters[i].members->data, clusters[i].members->len);
			g_array_sort(group->members, compare_index);
			g_ptr_array_add(groups, group);
		}
	}
	for (i = 0; i < n_clusters; i++)
		g_array_unref(clusters[i].members);
	return groups;
}

/* ── Readings ───────────────────────────────────────────────────── */

void
axis_identify_read(
	const AxisIdentifyName *folder,
	const AxisIdentifyName *file,
	AxisIdentifyReading    *reading
){
	gboolean f = folder != NULL && folder->key != NULL;
	gboolean m = file != NULL && file->key != NULL;

	memset(reading, 0, sizeof(*reading));
	if (folder == NULL) {
		if (m) {
			reading->kind = AXIS_IDENTIFY_READ_ALONE;
			reading->primary = file;
			reading->year = file->year;
		}
		return;
	}
	if (f && m) {
		if (strcmp(folder->key, file->key) == 0 &&
		    (folder->year == 0 || file->year == 0 || folder->year == file->year)) {
			reading->kind = AXIS_IDENTIFY_READ_AGREE;
			reading->primary = folder->year > 0 || file->year == 0 ? folder : file;
			reading->year = folder->year > 0 ? folder->year : file->year;
		} else {
			reading->kind = AXIS_IDENTIFY_READ_DISAGREE;
			reading->primary = folder;
			reading->secondary = file;
			reading->year = folder->year;
		}
	} else if (f) {
		reading->kind = AXIS_IDENTIFY_READ_FOLDER;
		reading->primary = folder;
		reading->year = folder->year;
	} else if (m) {
		reading->kind = AXIS_IDENTIFY_READ_FILE_ONLY;
		reading->primary = file;
		reading->year = file->year;
	}
}

const gchar *
axis_identify_read_label(AxisIdentifyReadKind kind)
{
	switch (kind) {
	case AXIS_IDENTIFY_READ_AGREE:
		return "the folder and the file, which agree";
	case AXIS_IDENTIFY_READ_FOLDER:
		return "the folder's name only (the file's name reads as no title)";
	case AXIS_IDENTIFY_READ_FILE_ONLY:
		return "the file's name only (the folder's name reads as no title)";
	case AXIS_IDENTIFY_READ_ALONE:
		return "the file's name";
	case AXIS_IDENTIFY_READ_DISAGREE:
		return "the folder and the file, which disagree";
	default:
		return "nothing";
	}
}

static gboolean
key_is(
	const gchar *key,
	const gchar *text
){
	g_autofree gchar *other = text != NULL ? axis_release_title_key(text) : NULL;

	return other != NULL && other[0] != '\0' && strcmp(key, other) == 0;
}

gboolean
axis_identify_title_fits(
	const AxisIdentifyName *name,
	const AxisMetaTitle    *title
){
	guint i;

	if (name == NULL || name->key == NULL || title == NULL)
		return FALSE;
	if (key_is(name->key, title->title) || key_is(name->key, title->original_title))
		return TRUE;
	for (i = 0; title->alt_titles != NULL && i < title->alt_titles->len; i++) {
		const AxisMetaAltTitle *alt = g_ptr_array_index(title->alt_titles, i);

		if (key_is(name->key, alt->title))
			return TRUE;
	}
	return FALSE;
}

/* How far @title's nearest year is from @year (0 when @year is 0), and
 * G_MAXINT when it has none. */
static gint
year_distance(
	const AxisMetaTitle *title,
	gint                 year
){
	gint years[3];
	guint n = axis_identify_title_years(title, years);
	gint best = G_MAXINT;
	guint i;

	if (n == 0)
		return G_MAXINT;
	if (year <= 0)
		return 0;
	for (i = 0; i < n; i++)
		best = MIN(best, ABS(years[i] - year));
	return best;
}

guint
axis_identify_named_order(
	const AxisIdentifyName     *name,
	const AxisMetaTitle *const *found,
	guint                       n,
	guint                      *order
){
	g_autofree gint *distance = g_new0(gint, MAX(n, 1));
	guint count = 0;
	guint i;

	for (i = 0; found != NULL && i < n; i++) {
		guint at;

		if (found[i] == NULL || !axis_identify_title_fits(name, found[i]))
			continue;
		distance[i] = year_distance(found[i], name->year);
		/* Insertion, stable: after every one as near. */
		at = count;
		while (at > 0 && distance[order[at - 1]] > distance[i]) {
			order[at] = order[at - 1];
			at--;
		}
		order[at] = i;
		count++;
	}
	return count;
}

static gboolean
name_disagrees(
	const AxisIdentifyName *name,
	const AxisMetaTitle    *title
){
	if (axis_identify_title_fits(name, title))
		return FALSE;
	return name->year > 0 && title->year > 0 && ABS(name->year - title->year) > 1;
}

gboolean
axis_identify_clearly_disagrees(
	const AxisIdentifyReading *reading,
	const AxisMetaTitle       *title
){
	gboolean any = FALSE;

	if (title == NULL)
		return FALSE;
	if (reading->primary != NULL && reading->primary->key != NULL) {
		if (!name_disagrees(reading->primary, title))
			return FALSE;
		any = TRUE;
	}
	if (reading->secondary != NULL && reading->secondary->key != NULL) {
		if (!name_disagrees(reading->secondary, title))
			return FALSE;
		any = TRUE;
	}
	return any;
}

/* ── Runtime ────────────────────────────────────────────────────── */

gboolean
axis_identify_runtime_fits(
	gint64  duration_ms,
	gint    runtime_minutes,
	gint    percent,
	gint    minutes,
	gint   *off_minutes
){
	gdouble off;
	gdouble tolerance;

	if (off_minutes != NULL)
		*off_minutes = 0;
	if (duration_ms <= 0 || runtime_minutes <= 0)
		return FALSE;
	off = (gdouble)duration_ms / 60000.0 - (gdouble)runtime_minutes;
	if (off < 0)
		off = -off;
	if (off_minutes != NULL)
		*off_minutes = off > 100000.0 ? 100000 : (gint)(off + 0.5);
	tolerance = MAX((gdouble)runtime_minutes * (gdouble)MAX(percent, 0) / 100.0, (gdouble)MAX(minutes, 0));
	return off <= tolerance;
}

gboolean
axis_identify_runtime_loose(
	gint64   duration_ms,
	gint     runtime_minutes,
	gboolean edition
){
	gdouble runs;
	gdouble band;
	gdouble longer;

	if (duration_ms <= 0 || runtime_minutes <= 0)
		return FALSE;
	runs = (gdouble)duration_ms / 60000.0;
	band = MAX((gdouble)runtime_minutes * AXIS_IDENTIFY_LOOSE_PERCENT / 100.0, (gdouble)AXIS_IDENTIFY_LOOSE_MINUTES);
	longer = edition ? MAX((gdouble)runtime_minutes * AXIS_IDENTIFY_EDITION_PERCENT / 100.0, band) : band;
	if (runs >= (gdouble)runtime_minutes)
		return runs - (gdouble)runtime_minutes <= longer;
	return (gdouble)runtime_minutes - runs <= band;
}

/* The year at the start of a stored date ("YYYY-MM-DD"), 0 for none. */
static gint
year_of_date(const gchar *date)
{
	gint year = 0;
	guint i;

	if (date == NULL)
		return 0;
	for (i = 0; i < 4; i++) {
		if (!g_ascii_isdigit(date[i]))
			return 0;
		year = year * 10 + (date[i] - '0');
	}
	return date[4] == '\0' || date[4] == '-' ? year : 0;
}

guint
axis_identify_title_years(
	const AxisMetaTitle *title,
	gint                 years[3]
){
	gint each[2];
	guint n = 0;
	guint i;
	guint k;

	if (title == NULL)
		return 0;
	each[0] = title->year > 0 ? title->year : year_of_date(title->first_air_date != NULL ? title->first_air_date :
		title->release_date);
	each[1] = title->regional_year;
	for (i = 0; i < 2; i++) {
		gboolean seen = FALSE;

		if (each[i] < AXIS_META_YEAR_MIN || each[i] > AXIS_META_YEAR_MAX)
			continue;
		for (k = 0; k < n; k++)
			seen = seen || years[k] == each[i];
		if (!seen)
			years[n++] = each[i];
	}
	return n;
}

/* ── A name that looks like part of a series ────────────────────── */

/* Whether @w (@n bytes) is an episode's or a special's number: "S05E01",
 * "S05M01" (a season, any letter, a number), or "5x01". */
static gboolean
series_mark_word(
	const gchar *w,
	gsize        n
){
	gsize i = 0;
	gsize digits = 0;

	if (n >= 4 && (w[0] == 'S' || w[0] == 's')) {
		for (i = 1; i < n && g_ascii_isdigit(w[i]); i++)
			digits++;
		if (digits < 1 || digits > 3 || i >= n || !g_ascii_isalpha(w[i]))
			return FALSE;
		for (i++, digits = 0; i < n && g_ascii_isdigit(w[i]); i++)
			digits++;
		return digits >= 1 && digits <= 4 && i == n;
	}
	for (; i < n && g_ascii_isdigit(w[i]); i++)
		digits++;
	if (digits < 1 || digits > 2 || i >= n || (w[i] != 'x' && w[i] != 'X'))
		return FALSE;
	for (i++, digits = 0; i < n && g_ascii_isdigit(w[i]); i++)
		digits++;
	return digits >= 2 && digits <= 3 && i == n;
}

gboolean
axis_identify_series_mark(
	const gchar  *title,
	gchar       **mark,
	gchar       **after
){
	const gchar *word;
	const gchar *end;

	if (mark != NULL)
		*mark = NULL;
	if (after != NULL)
		*after = NULL;
	if (title == NULL)
		return FALSE;
	/* Not the first word: a series' name comes before its number. */
	word = strchr(title, ' ');
	while (word != NULL) {
		word++;
		end = strchr(word, ' ');
		if (series_mark_word(word, end != NULL ? (gsize)(end - word) : strlen(word))) {
			if (mark != NULL)
				*mark = end != NULL ? g_strndup(word, (gsize)(end - word)) : g_strdup(word);
			if (after != NULL)
				*after = g_strstrip(g_strdup(end != NULL ? end : ""));
			return TRUE;
		}
		word = end;
	}
	return FALSE;
}

gchar *
axis_identify_search_words(
	AxisMetaKind  kind,
	const gchar  *read_title
){
	g_autofree gchar *after = NULL;

	if (kind != AXIS_META_KIND_SERIES && axis_identify_series_mark(read_title, NULL, &after) && after[0] != '\0')
		return g_steal_pointer(&after);
	return g_strdup(read_title);
}

/* ── The decision ───────────────────────────────────────────────── */

void
axis_identify_reason_free(AxisIdentifyReason *reason)
{
	if (reason == NULL)
		return;
	g_free(reason->code);
	g_free(reason->text);
	g_free(reason);
}

void
axis_identify_verdict_free(AxisIdentifyVerdict *verdict)
{
	if (verdict == NULL)
		return;
	g_ptr_array_unref(verdict->candidates);
	g_ptr_array_unref(verdict->reasons);
	g_free(verdict);
}

static void
reason(
	AxisIdentifyVerdict *verdict,
	const gchar         *code,
	const gchar         *format,
	...
) G_GNUC_PRINTF(3, 4);

static void
reason(
	AxisIdentifyVerdict *verdict,
	const gchar         *code,
	const gchar         *format,
	...
){
	AxisIdentifyReason *r = g_new0(AxisIdentifyReason, 1);
	va_list ap;

	va_start(ap, format);
	r->text = g_strdup_vprintf(format, ap);
	va_end(ap);
	r->code = g_strdup(code);
	g_ptr_array_add(verdict->reasons, r);
}

static void
candidate(
	AxisIdentifyVerdict *verdict,
	const AxisMetaTitle *title
){
	guint i;

	if (title == NULL || verdict->candidates->len >= AXIS_IDENTIFY_MAX_CANDIDATES)
		return;
	for (i = 0; i < verdict->candidates->len; i++) {
		const AxisMetaTitle *other = g_ptr_array_index(verdict->candidates, i);

		if (other == title || (other->id > 0 && other->id == title->id))
			return;
	}
	g_ptr_array_add(verdict->candidates, (gpointer)title);
}

static gchar *
label_of(const AxisMetaTitle *title)
{
	if (title == NULL || title->title == NULL)
		return g_strdup("a title");
	if (title->year > 0)
		return g_strdup_printf("%s (%d)", title->title, title->year);
	return g_strdup(title->title);
}

static gchar *
reading_words(const AxisIdentifyReading *reading)
{
	g_autofree gchar *a = axis_identify_name_describe(reading->primary);
	g_autofree gchar *b = NULL;

	if (reading->secondary == NULL)
		return g_steal_pointer(&a);
	b = axis_identify_name_describe(reading->secondary);
	return g_strdup_printf("%s and %s", a, b);
}

gboolean
axis_identify_id_decides(
	const AxisIdentifyIdEvidence *id,
	const AxisIdentifyReading    *reading
){
	return id->state == AXIS_IDENTIFY_ID_FOUND && id->title != NULL &&
		!axis_identify_clearly_disagrees(reading, id->title);
}

gboolean
axis_identify_corroborates(
	const AxisIdentifyReading *reading,
	const AxisMetaTitle       *title
){
	return axis_identify_title_fits(reading->primary, title) || axis_identify_title_fits(reading->secondary, title);
}

gboolean
axis_identify_nfo_decides(
	const AxisIdentifyIdEvidence *id,
	const AxisIdentifyReading    *reading
){
	return axis_identify_id_decides(id, reading) && axis_identify_corroborates(reading, id->title);
}

/* "movie" or "series", and the plural ("movies", "series"). */
static const gchar *
noun_of(AxisMetaKind kind)
{
	return kind == AXIS_META_KIND_SERIES ? "series" : "movie";
}

static const gchar *
nouns_of(AxisMetaKind kind)
{
	return kind == AXIS_META_KIND_SERIES ? "series" : "movies";
}

/*
 * An identifier's evidence: %TRUE when it settles the unit as sure.  Else its
 * reasons, its title as a candidate, and *@cap lowered when it stands
 * against the rest.  @weak: a problem of it does not lower the cap (an .nfo
 * that is stale or names two films is no evidence against the names), and
 * it is sure only when a name reads as the film's title (an .nfo may have
 * come from anywhere).
 */
static gboolean
weigh_id(
	AxisIdentifyVerdict          *verdict,
	const AxisIdentifyIdEvidence *id,
	const AxisIdentifyReading    *reading,
	const gchar                  *code,
	gboolean                      weak,
	AxisMetaKind                  kind,
	AxisIdentifyConfidence       *cap,
	gboolean                     *from_id
){
	const gchar *where = id->where != NULL ? id->where : "a name";
	const gchar *token = id->token != NULL ? id->token : "an identifier";

	switch (id->state) {
	case AXIS_IDENTIFY_ID_CONFLICT:
		reason(verdict, "id_conflict", "%c%s carries two different identifiers of one kind: neither is believed",
			g_ascii_toupper(where[0]), where + 1);
		if (!weak)
			*cap = AXIS_IDENTIFY_LIKELY;
		return FALSE;
	case AXIS_IDENTIFY_ID_NOT_FOUND:
		reason(verdict, "id_unknown", "the identifier %s in %s names no %s the details provider knows", token, where,
			noun_of(kind));
		if (!weak)
			*cap = AXIS_IDENTIFY_LIKELY;
		return FALSE;
	case AXIS_IDENTIFY_ID_FOUND:
		if (id->title == NULL)
			return FALSE;
		{
			g_autofree gchar *label = label_of(id->title);

			if (axis_identify_clearly_disagrees(reading, id->title)) {
				g_autofree gchar *words = reading_words(reading);

				reason(verdict, "id_disagrees", "the identifier %s in %s names %s, but the names read as %s", token,
					where, label, words);
				candidate(verdict, id->title);
				*cap = AXIS_IDENTIFY_LIKELY;
				*from_id = TRUE;
				return FALSE;
			}
			if (weak && !axis_identify_corroborates(reading, id->title)) {
				g_autofree gchar *words = reading_words(reading);

				if (reading->primary == NULL)
					reason(verdict, "id_uncorroborated", "the identifier %s in %s names %s, but the names give "
						"nothing to check it against", token, where, label);
				else
					reason(verdict, "id_uncorroborated", "the identifier %s in %s names %s, but no name reads as its "
						"title (they read as %s)", token, where, label, words);
				candidate(verdict, id->title);
				*cap = AXIS_IDENTIFY_LIKELY;
				*from_id = TRUE;
				return FALSE;
			}
			if (*cap != AXIS_IDENTIFY_SURE) {
				reason(verdict, code, "the identifier %s in %s names %s", token, where, label);
				candidate(verdict, id->title);
				*from_id = TRUE;
				return FALSE;
			}
			reason(verdict, code, "identifier %s in %s: %s", token, where, label);
			candidate(verdict, id->title);
			verdict->confidence = AXIS_IDENTIFY_SURE;
			verdict->title = id->title;
			return TRUE;
		}
	default:
		return FALSE;
	}
}

/* The candidates of @found that @name fits; how many. */
static guint
fitting(
	AxisIdentifyVerdict        *verdict,
	const AxisIdentifyName     *name,
	const AxisMetaTitle *const *found,
	guint                       n
){
	guint fits = 0;
	guint i;

	for (i = 0; i < n; i++) {
		if (axis_release_match(name->release, &found[i], 1, NULL, NULL) != AXIS_RELEASE_MATCH_NONE) {
			candidate(verdict, found[i]);
			fits++;
		}
	}
	return fits;
}

static void
first_found(
	AxisIdentifyVerdict        *verdict,
	const AxisMetaTitle *const *found,
	guint                       n
){
	guint i;

	for (i = 0; i < n && i < 5; i++)
		candidate(verdict, found[i]);
}

/* ── A series' folder with no year: its episodes ────────────────── */

/* What one series of the name is to the episodes on disk (rule 5). */
typedef enum
{
	SERIES_FITS,
	SERIES_NO_FIT,		/* the episodes or the runtime rule it out */
	SERIES_UNCHECKED	/* nothing to check them against: it cannot be ruled out */
} SeriesFit;

/* One series of the name against the episodes on disk (rule 5), with the
 * reason either way. */
static SeriesFit
compare_series(
	AxisIdentifyVerdict               *verdict,
	const AxisIdentifyEvidence        *e,
	const AxisIdentifySeriesCandidate *c
){
	g_autofree gchar *label = label_of(c->title);
	g_autofree gchar *runtime_words = NULL;
	AxisIdentifyFit fit;
	gint runtime = c->episode_runtime > 0 ? c->episode_runtime : c->title->runtime;
	gint off = 0;

	axis_identify_shape_fit(e->shape, c->title, &fit);
	if (!fit.listed) {
		reason(verdict, "no_seasons", "the details provider lists no seasons of %s, so the episodes on disk cannot be checked "
			"against it", label);
		return SERIES_UNCHECKED;
	}
	if (fit.missing > 0) {
		reason(verdict, "season_missing", "%s has no season %d, and %u of the episodes on disk %s", label,
			fit.missing_season, fit.missing, fit.missing == 1 ? "is in one it does not have" :
			"are in seasons it does not have");
		return SERIES_NO_FIT;
	}
	if (fit.checked < AXIS_IDENTIFY_SHAPE_MIN_EPISODES) {
		reason(verdict, "episodes_unknown", "the details provider does not say how many episodes the seasons of %s have, so "
			"the episodes on disk cannot be checked against it", label);
		return SERIES_UNCHECKED;
	}
	if (!fit.fits) {
		reason(verdict, "episodes_off", "%u of the %u numbered episodes on disk are not episodes of %s (it has no "
			"S%02dE%02d)", fit.checked - fit.landed, fit.checked, label, fit.off_season, fit.off_number);
		return SERIES_NO_FIT;
	}
	if (e->duration_ms > 0 && runtime > 0 &&
	    !axis_identify_runtime_fits(e->duration_ms, runtime, e->percent, e->minutes, &off)) {
		reason(verdict, "runtime_off", "the episodes on disk fit %s, but its episode runs %d minutes and the main "
			"video %" G_GINT64_FORMAT ": %d minutes off", label, runtime, (e->duration_ms + 30000) / 60000, off);
		return SERIES_NO_FIT;
	}
	if (e->duration_ms <= 0)
		runtime_words = g_strdup("");
	else if (runtime <= 0)
		runtime_words = g_strdup("; the details provider gives no runtime for its episodes, so that is not checked");
	else
		runtime_words = g_strdup_printf("; the main video runs %" G_GINT64_FORMAT " minutes against %d for the "
			"episode", (e->duration_ms + 30000) / 60000, runtime);
	reason(verdict, "episodes_fit", "%s fits: %u of the %u numbered episodes on disk are its episodes%s%s", label,
		fit.landed, fit.checked, fit.unchecked > 0 ? " (the rest are in seasons whose size it does not give)" : "",
		runtime_words);
	return SERIES_FITS;
}

/* "A (2010)", "A (2010) and B (1998)", "A, B and C": the series of @which. */
static gchar *
series_names(
	const AxisIdentifyEvidence *e,
	const SeriesFit            *fits,
	SeriesFit                   which
){
	GString *names = g_string_new(NULL);
	guint count = 0;
	guint named = 0;
	guint i;

	for (i = 0; i < e->n_series; i++)
		count += fits[i] == which;
	for (i = 0; i < e->n_series; i++) {
		g_autofree gchar *label = NULL;

		if (fits[i] != which)
			continue;
		label = label_of(e->series[i].title);
		g_string_append_printf(names, "%s%s", named > 0 ? (named + 1 == count ? " and " : ", ") : "", label);
		named++;
	}
	return g_string_free(names, FALSE);
}

/*
 * A series' folder whose names give no year (rule 5): the series of that
 * name compared with the episodes on disk.  Sure only when exactly one fits,
 * no other was left out or cannot be checked, and everything else needed is
 * there.
 */
static AxisIdentifyConfidence
weigh_series(
	AxisIdentifyVerdict        *verdict,
	const AxisIdentifyEvidence *e
){
	const AxisIdentifyReading *r = &e->reading;
	g_autofree SeriesFit *fits = g_new0(SeriesFit, MAX(e->n_series, 1));
	g_autofree gchar *first = label_of(e->series[0].title);
	guint numbered = MIN(e->shape->episodes->len, e->shape->n_numbered);
	guint n_fit = 0;
	guint n_unchecked = 0;
	gint chosen = -1;
	guint i;

	const gchar *from = r->kind == AXIS_IDENTIFY_READ_AGREE ? "the folder and its episode files, which agree" :
		r->kind == AXIS_IDENTIFY_READ_FOLDER ? "the folder's name (no episode file names a series)" :
		r->kind == AXIS_IDENTIFY_READ_FILE_ONLY ? "the episode files (the folder's name reads as no title)" :
		axis_identify_read_label(r->kind);

	if (e->n_series == 1)
		reason(verdict, "names", "title from %s: %s, the one series of that name%s", from, first,
			e->series[0].other_name ? " (it is one of its other names)" : "");
	else
		reason(verdict, "names", "title from %s: %u%s series have that name", from, e->n_series,
			e->series_more ? " or more" : "");
	reason(verdict, "series_no_year", "the names give no year, so the episodes on disk are checked against the "
		"seasons and episodes of %s instead", e->n_series == 1 ? "that series" : "each of them");
	if (numbered < AXIS_IDENTIFY_SHAPE_MIN_EPISODES) {
		g_autofree gchar *few = numbered == 0 ? g_strdup("no file on disk is a numbered episode") :
			g_strdup_printf("only %u numbered %s on disk", numbered, numbered == 1 ? "episode is" : "episodes are");

		reason(verdict, "few_episodes", "%s, and at least %d, in as many files, are needed for that check", few,
			AXIS_IDENTIFY_SHAPE_MIN_EPISODES);
		for (i = 0; i < e->n_series; i++)
			candidate(verdict, e->series[i].title);
		return AXIS_IDENTIFY_LIKELY;
	}
	for (i = 0; i < e->n_series; i++) {
		fits[i] = compare_series(verdict, e, &e->series[i]);
		if (fits[i] == SERIES_FITS) {
			n_fit++;
			chosen = (gint)i;
		} else if (fits[i] == SERIES_UNCHECKED) {
			n_unchecked++;
		}
	}
	/* The one that fits first, then those that cannot be checked, then the
	 * others in the provider's order. */
	if (n_fit == 1)
		candidate(verdict, e->series[chosen].title);
	for (i = 0; i < e->n_series; i++) {
		if (fits[i] == SERIES_UNCHECKED)
			candidate(verdict, e->series[i].title);
	}
	for (i = 0; i < e->n_series; i++)
		candidate(verdict, e->series[i].title);
	if (n_fit == 0) {
		if (n_unchecked > 0) {
			g_autofree gchar *names = series_names(e, fits, SERIES_UNCHECKED);

			reason(verdict, "no_fit", e->n_series == 1 ? "the episodes on disk cannot be checked against that series" :
				"the episodes on disk fit none of them that can be checked, and %s cannot be", names);
		} else {
			reason(verdict, "no_fit", e->n_series == 1 ? "the episodes on disk do not fit that series" :
				"the episodes on disk fit none of them");
		}
		return AXIS_IDENTIFY_LIKELY;
	}
	if (n_fit > 1) {
		g_autofree gchar *names = series_names(e, fits, SERIES_FITS);

		reason(verdict, "series_ambiguous", "%u series of that name fit the episodes on disk (%s), so neither is "
			"taken", n_fit, names);
		return AXIS_IDENTIFY_LIKELY;
	}
	{
		g_autofree gchar *label = label_of(e->series[chosen].title);

		if (n_unchecked > 0) {
			g_autofree gchar *names = series_names(e, fits, SERIES_UNCHECKED);

			reason(verdict, "series_unchecked", "%s fits, but the episodes on disk cannot be checked against %s, "
				"which %s that name too, so %s is not taken as sure", label, names, n_unchecked == 1 ? "has" : "have",
				label);
			return AXIS_IDENTIFY_LIKELY;
		}
		if (e->series[chosen].other_name && e->others_partial) {
			reason(verdict, "other_name_partial", "the folder's name is only one of the other names of %s, and only the "
				"first answers of the search were read for their other names, so it is not taken as sure", label);
			return AXIS_IDENTIFY_LIKELY;
		}
		if (e->series_more) {
			reason(verdict, "more_series", "more than %d series have that name and only %d were compared, so %s is "
				"not taken as sure", AXIS_IDENTIFY_SERIES_COMPARED, AXIS_IDENTIFY_SERIES_COMPARED, label);
			return AXIS_IDENTIFY_LIKELY;
		}
		if (r->kind != AXIS_IDENTIFY_READ_AGREE && r->kind != AXIS_IDENTIFY_READ_FOLDER) {
			reason(verdict, r->kind == AXIS_IDENTIFY_READ_FILE_ONLY ? "file_only" : "alone", "%s",
				r->kind == AXIS_IDENTIFY_READ_FILE_ONLY ? "only the files' names read as a title (not the folder's), "
				"so it is not taken as sure" : "it is a file at the top of the root, not a series' folder, so it is "
				"not taken as sure");
			return AXIS_IDENTIFY_LIKELY;
		}
		if (e->duration_ms <= 0) {
			reason(verdict, "no_probe", "the main video is not read yet, so its runtime is not checked");
			return AXIS_IDENTIFY_LIKELY;
		}
		if (e->n_series == 1)
			reason(verdict, "series_fits", "%s is the one series of that name, and the episodes on disk fit it, so no "
				"year is needed", label);
		else
			reason(verdict, "series_fits", "%s is the only series of that name that the episodes on disk fit, so no "
				"year is needed", label);
	}
	return AXIS_IDENTIFY_SURE;
}

/* ── The titles of the name read (rule 4) ───────────────────────── */

/* Whether @a and @b are one title: the same record, or the same stored id. */
static gboolean
same_title(
	const AxisMetaTitle *a,
	const AxisMetaTitle *b
){
	return a != NULL && b != NULL && (a == b || (a->id > 0 && a->id == b->id));
}

/* @title's full record when the evidence holds one read for the decision,
 * else @title as it is stored. */
static const AxisMetaTitle *
record_of(
	const AxisIdentifyEvidence *e,
	const AxisMetaTitle        *title
){
	guint i;

	for (i = 0; e->details != NULL && i < e->n_details; i++) {
		if (same_title(e->details[i], title))
			return e->details[i];
	}
	if (same_title(e->detail, title))
		return e->detail;
	return title;
}

/* The runtime the main video should have if it is @record: a series' main
 * video runs as the episode its name holds, else as the series' episodes
 * usually do. */
static gint
runtime_of(
	const AxisIdentifyEvidence *e,
	const AxisMetaTitle        *record
){
	guint i;

	if (e->kind == AXIS_META_KIND_SERIES) {
		for (i = 0; e->series != NULL && i < e->n_series; i++) {
			if (same_title(e->series[i].title, record) && e->series[i].episode_runtime > 0)
				return e->series[i].episode_runtime;
		}
		if (e->episode_runtime > 0 && (e->detail == NULL || same_title(e->detail, record)))
			return e->episode_runtime;
	}
	return MAX(record->runtime, 0);
}

typedef enum
{
	YEAR_NOT_GIVEN = 0,	/* the names give no year */
	YEAR_EXACT,		/* one of its release years is the names' */
	YEAR_NEAR,		/* none is, one is a year off */
	YEAR_TWO,		/* none is a year off, one is two (a festival's year, a
				 * first showing elsewhere): only ever likely */
	YEAR_NOT_KNOWN,		/* it has no year known */
	YEAR_FAR		/* each is further off: another title of the name */
} YearFit;

typedef enum
{
	RUN_NOT_KNOWN = 0,	/* no probe yet, or no runtime given */
	RUN_FITS,		/* within the tolerance */
	RUN_LOOSE,		/* within the wider band only (a film's) */
	RUN_OFF
} RunFit;

/* A title of the name read, and what the names and the file say of it. */
typedef struct
{
	const AxisMetaTitle *record;	/* its full record when read, else as stored */
	YearFit              year;
	gint                 first;	/* its first release's year, 0 for none known */
	gboolean             regional;	/* exact by its theatrical release in the region only */
	gboolean             by_other;	/* the name is only one of its other names */
	gint                 runtime;
	RunFit               run;
	gint                 off;
} Named;

/* @record against the names' @year: exact by its first release or by its
 * theatrical release in the region (*@regional), a year off by either, two
 * off, or further. */
static YearFit
year_fit(
	gint                 year,
	const AxisMetaTitle *record,
	gint                *first,
	gboolean            *regional
){
	gint years[3];
	guint n = axis_identify_title_years(record, years);
	gboolean near = FALSE;
	gboolean two = FALSE;
	guint i;

	*regional = FALSE;
	*first = n > 0 ? years[0] : 0;
	if (year <= 0)
		return YEAR_NOT_GIVEN;
	if (n == 0)
		return YEAR_NOT_KNOWN;
	for (i = 0; i < n; i++) {
		if (years[i] == year) {
			*regional = i > 0;
			return YEAR_EXACT;
		}
		near = near || ABS(years[i] - year) == 1;
		two = two || ABS(years[i] - year) == 2;
	}
	return near ? YEAR_NEAR : two ? YEAR_TWO : YEAR_FAR;
}

/* Whether @name has @title's own title or original title (not only one of
 * its other names). */
static gboolean
name_is_title(
	const AxisIdentifyName *name,
	const AxisMetaTitle    *title
){
	return name != NULL && name->key != NULL && title != NULL &&
		(key_is(name->key, title->title) || key_is(name->key, title->original_title));
}

/* "Tinder Hollow (2014, 98 minutes)", "Tinder Hollow (2013 or 2014, no
 * runtime known)". */
static gchar *
named_label(const Named *c)
{
	g_autofree gchar *runs = c->runtime > 0 ? g_strdup_printf("%d minutes", c->runtime) : g_strdup("no runtime known");
	const gchar *name = c->record->title != NULL ? c->record->title : "a title";
	gint years[3];
	guint n = axis_identify_title_years(c->record, years);

	if (n == 0)
		return g_strdup_printf("%s (no year known, %s)", name, runs);
	if (n == 1)
		return g_strdup_printf("%s (%d, %s)", name, years[0], runs);
	return g_strdup_printf("%s (%d or %d, %s)", name, years[0], years[1], runs);
}

/* The titles of @named marked in @which, in words: "A (...) and B (...)". */
static gchar *
named_list(
	const Named    *named,
	guint           n,
	const gboolean *which
){
	GString *out = g_string_new(NULL);
	guint count = 0;
	guint done = 0;
	guint i;

	for (i = 0; i < n; i++)
		count += which[i] != FALSE;
	for (i = 0; i < n; i++) {
		g_autofree gchar *label = NULL;

		if (!which[i])
			continue;
		label = named_label(&named[i]);
		g_string_append_printf(out, "%s%s", done > 0 ? (done + 1 == count ? " and " : ", ") : "", label);
		done++;
	}
	return g_string_free(out, FALSE);
}

/*
 * The names against the titles the provider has of that name (rule 4),
 * each judged by its full record: which one the year and the runtime say it
 * is, if one, and what keeps it from being sure.  Never the provider's
 * order or popularity: titles they do not tell apart stay ambiguous.
 */
static AxisIdentifyConfidence
weigh_titles(
	AxisIdentifyVerdict        *verdict,
	const AxisIdentifyEvidence *e
){
	const AxisIdentifyReading *r = &e->reading;
	const gchar *noun = noun_of(e->kind);
	const gchar *nouns = nouns_of(e->kind);
	gboolean longer = (e->editions & AXIS_IDENTIFY_EDITION_LONGER) != 0;
	g_autofree Named *named = g_new0(Named, MAX(e->n_found, 1));
	g_autofree gboolean *marked = g_new0(gboolean, MAX(e->n_found, 1));
	g_autofree gchar *words = axis_identify_name_describe(r->primary);
	g_autofree gchar *label = NULL;
	AxisIdentifyConfidence confidence = AXIS_IDENTIFY_SURE;
	const AxisMetaTitle *far = NULL;
	const Named *chosen = NULL;
	gint64 runs = (e->duration_ms + 30000) / 60000;
	guint n = 0;
	guint n_exact = 0;
	guint n_near = 0;
	guint n_two = 0;
	guint n_unknown = 0;
	guint could = 0;
	gboolean near_blocks = FALSE;
	guint i;

	/* The titles of the name read, each once, by their full records. */
	for (i = 0; e->found != NULL && i < e->n_found; i++) {
		const AxisMetaTitle *found = e->found[i];
		const AxisMetaTitle *record;
		Named *c;
		gboolean seen = FALSE;
		guint k;

		if (found == NULL)
			continue;
		record = record_of(e, found);
		if (!axis_identify_title_fits(r->primary, record) && !axis_identify_title_fits(r->primary, found))
			continue;
		for (k = 0; k < n && !seen; k++)
			seen = same_title(named[k].record, record);
		if (seen)
			continue;
		c = &named[n];
		memset(c, 0, sizeof(*c));
		c->record = record;
		c->year = year_fit(r->year, record, &c->first, &c->regional);
		if (c->year == YEAR_FAR) {
			if (far == NULL)
				far = record;
			continue;
		}
		c->by_other = !name_is_title(r->primary, record) && !name_is_title(r->primary, found);
		c->runtime = runtime_of(e, record);
		c->run = RUN_NOT_KNOWN;
		if (e->duration_ms > 0 && c->runtime > 0) {
			if (axis_identify_runtime_fits(e->duration_ms, c->runtime, e->percent, e->minutes, &c->off))
				c->run = RUN_FITS;
			else if (e->kind != AXIS_META_KIND_SERIES && axis_identify_runtime_loose(e->duration_ms, c->runtime, longer))
				c->run = RUN_LOOSE;
			else
				c->run = RUN_OFF;
		}
		n++;
	}
	/* The year of a theatrical release in the region is the year only when no
	 * other title of the name was first released within a year of the
	 * names' year: else it is no better than a year off. */
	for (i = 0; i < n; i++) {
		guint k;

		if (named[i].year != YEAR_EXACT || !named[i].regional)
			continue;
		for (k = 0; k < n; k++) {
			if (k != i && named[k].first > 0 && ABS(named[k].first - r->year) <= 1) {
				named[i].year = YEAR_NEAR;
				break;
			}
		}
	}
	for (i = 0; i < n; i++) {
		n_exact += named[i].year == YEAR_EXACT;
		n_near += named[i].year == YEAR_NEAR;
		n_two += named[i].year == YEAR_TWO;
		n_unknown += named[i].year == YEAR_NOT_KNOWN;
	}
	if (n == 0) {
		g_autofree gchar *mark = NULL;
		g_autofree gchar *after = NULL;

		/* A film's name that holds an episode's or a special's number: say
		 * so, and what the search box will hold (fix-identify-regional-years,
		 * C). */
		if (e->kind != AXIS_META_KIND_SERIES && r->primary != NULL &&
		    axis_identify_series_mark(r->primary->release->title, &mark, &after)) {
			if (after[0] != '\0')
				reason(verdict, "series_like", "the name looks like part of a series: %s in it reads as the number of "
					"an episode or a special, so it may be no %s of its own; the search below looks for the words after "
					"it, \"%s\"", mark, noun, after);
			else
				reason(verdict, "series_like", "the name looks like part of a series: %s in it reads as the number of "
					"an episode or a special, so it may be no %s of its own", mark, noun);
		}
		if (far != NULL) {
			g_autofree gchar *other = label_of(far);

			reason(verdict, "no_match", "the details provider knows no %s that fits %s (%s has that title, but the names say "
				"%d)", noun, words, other, r->year);
		} else {
			reason(verdict, "no_match", "the details provider knows no %s that fits %s", noun, words);
		}
		first_found(verdict, e->found, e->n_found);
		return AXIS_IDENTIFY_UNKNOWN;
	}

	/* Which one it is.  Without a year: the one title of the name.  With a
	 * year: the one that has it exactly and whose runtime fits or is not
	 * known (one a year off, or with no year known, does not stand in its
	 * way); when the runtime rules out every one of the year, one a year
	 * off whose runtime fits stands against them.  Else the one a year off,
	 * alone; else the one two off, alone; else the one with no year known,
	 * alone.  A title two off never stands in the way of one nearer.
	 * Marked: those that cannot be told apart. */
	if (r->year <= 0) {
		if (n == 1)
			chosen = &named[0];
		for (i = 0; n > 1 && i < n; i++)
			marked[i] = TRUE;
	} else if (n_exact > 0) {
		for (i = 0; i < n; i++) {
			if (named[i].year == YEAR_EXACT && (named[i].run == RUN_FITS || named[i].run == RUN_NOT_KNOWN)) {
				marked[i] = TRUE;
				could++;
			}
		}
		for (i = 0; could == 0 && i < n; i++) {
			if (named[i].year == YEAR_EXACT || (named[i].year == YEAR_NEAR && named[i].run == RUN_FITS))
				marked[i] = TRUE;
		}
		could = 0;
		for (i = 0; i < n; i++)
			could += marked[i] != FALSE;
		for (i = 0; could == 1 && i < n; i++) {
			if (marked[i])
				chosen = &named[i];
		}
		/* One of the year whose runtime fits does not rule out one a year
		 * off (a release's year may be its festival's), unless that one's
		 * runtime is known and does not fit, by the tolerance itself. */
		if (chosen != NULL && chosen->run == RUN_FITS) {
			for (i = 0; i < n; i++) {
				if (named[i].year == YEAR_NEAR && (named[i].run == RUN_FITS || named[i].run == RUN_NOT_KNOWN)) {
					marked[i] = TRUE;
					near_blocks = TRUE;
				}
			}
			if (near_blocks)
				chosen = NULL;
		}
	} else if (n_near > 0) {
		for (i = 0; i < n; i++) {
			marked[i] = named[i].year == YEAR_NEAR || named[i].year == YEAR_NOT_KNOWN;
			if (n_near == 1 && n_unknown == 0 && named[i].year == YEAR_NEAR)
				chosen = &named[i];
		}
	} else if (n_two > 0) {
		/* Two off at the nearest (a festival's year, a first showing
		 * elsewhere): the one such, alone, is likely, never sure. */
		for (i = 0; i < n; i++) {
			marked[i] = named[i].year == YEAR_TWO || named[i].year == YEAR_NOT_KNOWN;
			if (n_two == 1 && n_unknown == 0 && named[i].year == YEAR_TWO)
				chosen = &named[i];
		}
	} else {
		if (n == 1)
			chosen = &named[0];
		for (i = 0; n > 1 && i < n; i++)
			marked[i] = TRUE;
	}

	if (chosen == NULL) {
		g_autofree gchar *list = named_list(named, n, marked);
		guint count = 0;

		for (i = 0; i < n; i++) {
			count += marked[i] != FALSE;
			if (marked[i])
				candidate(verdict, named[i].record);
		}
		for (i = 0; i < n; i++)
			candidate(verdict, named[i].record);
		if (r->year <= 0)
			reason(verdict, "ambiguous", "%u %s fit %s: %s; the names give no year to tell them apart", count, nouns,
				words, list);
		else if (near_blocks)
			reason(verdict, "ambiguous", "%u %s fit %s: %s; the file runs %" G_GINT64_FORMAT " minutes, and one a year "
				"off is not ruled out by its runtime (a release's year may be its festival's)", count, nouns, words, list,
				runs);
		else if (e->duration_ms <= 0)
			reason(verdict, "ambiguous", "%u %s fit %s: %s; the file is not read yet, so its runtime cannot tell "
				"them apart", count, nouns, words, list);
		else
			reason(verdict, "ambiguous", "%u %s fit %s: %s; the file runs %" G_GINT64_FORMAT " minutes, and the year "
				"and the runtime do not tell them apart", count, nouns, words, list, runs);
		return AXIS_IDENTIFY_LIKELY;
	}

	candidate(verdict, chosen->record);
	for (i = 0; i < n; i++)
		candidate(verdict, named[i].record);
	label = label_of(chosen->record);
	reason(verdict, "names", "%s from %s: %s", r->year > 0 ? "title and year" : "title",
		axis_identify_read_label(r->kind), label);
	if (n > 1) {
		g_autofree gboolean *others = g_new0(gboolean, n);
		g_autofree gchar *list = NULL;

		for (i = 0; i < n; i++)
			others[i] = &named[i] != chosen;
		list = named_list(named, n, others);
		if (chosen->year == YEAR_EXACT)
			reason(verdict, "others_ruled_out", "%s is taken over the other %s of that name, %s: only it has the year "
				"%d%s", label, n == 2 ? noun : nouns, list, r->year, chosen->run == RUN_FITS ?
				" with a runtime that fits" : "");
		else
			reason(verdict, "others_ruled_out", "%s is taken over the other %s of that name, %s: its year is the "
				"nearest to %d", label, n == 2 ? noun : nouns, list, r->year);
	}

	/* One name alone is enough for one video (a film's folder or file), or
	 * for a series' folder whose episode files name no series. */
	if (r->kind == AXIS_IDENTIFY_READ_FOLDER || r->kind == AXIS_IDENTIFY_READ_FILE_ONLY) {
		gboolean enough = e->kind == AXIS_META_KIND_SERIES ? r->kind == AXIS_IDENTIFY_READ_FOLDER : e->n_videos == 1;

		if (!enough) {
			reason(verdict, r->kind == AXIS_IDENTIFY_READ_FOLDER ? "folder_only" : "file_only", "%s",
				e->kind == AXIS_META_KIND_SERIES ? "only the episode files' names read as a title, not the folder's, so "
				"it is not taken as sure" : "only one name reads as a title and the folder holds several videos, so it "
				"is not taken as sure");
			confidence = AXIS_IDENTIFY_LIKELY;
		}
	}

	if (r->year <= 0) {
		reason(verdict, "no_year", "the names give no year");
		confidence = AXIS_IDENTIFY_LIKELY;
	} else if (chosen->year == YEAR_NEAR) {
		gint years[3];
		guint n_years = axis_identify_title_years(chosen->record, years);

		if (chosen->regional)
			reason(verdict, "year_off", "%d is the year of its theatrical release in the region, not of its first "
				"release (%d), and another %s of that name came out within a year of %d, so that counts as a year off",
				r->year, chosen->first, noun, r->year);
		else if (n_years > 1)
			reason(verdict, "year_off", "the year is one off: %d in the names, %d for the %s (%d in the region)",
				r->year, years[0], noun, years[1]);
		else
			reason(verdict, "year_off", "the year is one off: %d in the names, %d for the %s", r->year, years[0], noun);
		confidence = AXIS_IDENTIFY_LIKELY;
	} else if (chosen->year == YEAR_TWO) {
		gint years[3];
		guint n_years = axis_identify_title_years(chosen->record, years);

		if (n_years > 1)
			reason(verdict, "year_off", "the year is two off: %d in the names, %d for the %s (%d in the region); a "
				"festival or a first showing elsewhere may give the names' year", r->year, years[0], noun, years[1]);
		else
			reason(verdict, "year_off", "the year is two off: %d in the names, %d for the %s; a festival or a first "
				"showing elsewhere may give the names' year", r->year, years[0], noun);
		confidence = AXIS_IDENTIFY_LIKELY;
	} else if (chosen->year == YEAR_NOT_KNOWN) {
		reason(verdict, "year_unknown", "the details provider gives no year for the %s, so the year in the names is not checked",
			noun);
		confidence = AXIS_IDENTIFY_LIKELY;
	} else if (chosen->regional) {
		reason(verdict, "year_regional", "%d is the year of its theatrical release in the region; it was first "
			"released in %d, and no other %s of that name came out within a year of %d", r->year, chosen->first, noun,
			r->year);
	}
	/* Found by another name among the first answers read in full, when not
	 * every answer was: another answer could have that name too. */
	if (chosen->by_other && e->others_partial) {
		reason(verdict, "other_name_partial", "the names give %s only as one of its other names, and only the first "
			"answers of the search were read for their other names, so it is not taken as sure", label);
		confidence = AXIS_IDENTIFY_LIKELY;
	}
	if (e->details_more) {
		reason(verdict, "more_titles", "more than %d %s have that name and only %d were read in full, so %s is not "
			"taken as sure", AXIS_IDENTIFY_DETAILS, nouns, AXIS_IDENTIFY_DETAILS, label);
		confidence = AXIS_IDENTIFY_LIKELY;
	}

	if (e->duration_ms <= 0) {
		reason(verdict, "no_probe", "the file is not read yet, so its runtime is not checked");
		return AXIS_IDENTIFY_LIKELY;
	}
	if (chosen->runtime <= 0) {
		if (e->kind == AXIS_META_KIND_SERIES)
			reason(verdict, "no_runtime", "the details provider gives no runtime for the series' episodes, so it is not "
				"checked");
		else
			reason(verdict, "no_runtime", "the details provider gives no runtime for the movie, so it is not checked");
		return AXIS_IDENTIFY_LIKELY;
	}
	if (chosen->run == RUN_FITS) {
		reason(verdict, "runtime_ok", "runtime within tolerance: %" G_GINT64_FORMAT " minutes against %d%s", runs,
			chosen->runtime, e->kind == AXIS_META_KIND_SERIES ? " for the episode" : "");
		return confidence;
	}
	/* The wider band: the one film of its name among the answers (no
	 * namesake of any year), by its first release's year and its own title,
	 * named so by the folder and the file agreeing (or by a file standing
	 * alone). */
	if (chosen->run == RUN_LOOSE && chosen->year == YEAR_EXACT && !chosen->regional && !chosen->by_other && n == 1 &&
	    far == NULL && !e->details_more && (r->kind == AXIS_IDENTIFY_READ_AGREE || r->kind == AXIS_IDENTIFY_READ_ALONE)) {
		reason(verdict, "runtime_loose", "runtime %d minutes off: %" G_GINT64_FORMAT " minutes against %d, within the "
			"wider band for the one %s of that name%s", chosen->off, runs, chosen->runtime, noun,
			longer && runs > chosen->runtime ? " (its name says it is another cut, which runs longer)" : "");
		return confidence;
	}
	reason(verdict, "runtime_off", "runtime %d minutes off: %" G_GINT64_FORMAT " minutes against %d%s%s", chosen->off,
		runs, chosen->runtime, e->kind == AXIS_META_KIND_SERIES ? " for the episode" : "",
		chosen->run != RUN_LOOSE ? "" : n > 1 || far != NULL ?
		" (within the wider band, which holds only for the one title of a name)" :
		" (within the wider band, which holds when the folder and the file agree on the title and the year of its "
		"first release)");
	return AXIS_IDENTIFY_LIKELY;
}

/* The names alone: what they read as, against the provider's titles. */
static AxisIdentifyConfidence
weigh_names(
	AxisIdentifyVerdict        *verdict,
	const AxisIdentifyEvidence *e
){
	const AxisIdentifyReading *r = &e->reading;
	const gchar *noun = noun_of(e->kind);

	if (r->kind == AXIS_IDENTIFY_READ_NONE || r->primary == NULL) {
		reason(verdict, "no_title", "nothing in the names reads as a title");
		return AXIS_IDENTIFY_UNKNOWN;
	}
	if (r->kind == AXIS_IDENTIFY_READ_DISAGREE) {
		g_autofree gchar *a = axis_identify_name_describe(r->primary);
		g_autofree gchar *b = axis_identify_name_describe(r->secondary);
		guint fits;

		reason(verdict, "disagree", "the folder reads as %s and the file as %s", a, b);
		fits = fitting(verdict, r->primary, e->found, e->n_found) +
			fitting(verdict, r->secondary, e->found, e->n_found);
		if (fits > 0)
			return AXIS_IDENTIFY_LIKELY;
		reason(verdict, "no_match", "the details provider knows no %s of either", noun);
		first_found(verdict, e->found, e->n_found);
		return AXIS_IDENTIFY_UNKNOWN;
	}
	/* A series' folder with no year: by its episodes (rule 5). */
	if (e->kind == AXIS_META_KIND_SERIES && r->year == 0 && e->shape != NULL && e->shape->episodes != NULL &&
	    e->series != NULL && e->n_series > 0)
		return weigh_series(verdict, e);

	return weigh_titles(verdict, e);
}

static void
decide_into(
	AxisIdentifyVerdict        *verdict,
	const AxisIdentifyEvidence *e
);

AxisIdentifyVerdict *
axis_identify_decide(const AxisIdentifyEvidence *e)
{
	AxisIdentifyVerdict *verdict = g_new0(AxisIdentifyVerdict, 1);

	verdict->candidates = g_ptr_array_new();
	verdict->reasons = g_ptr_array_new_with_free_func((GDestroyNotify)axis_identify_reason_free);
	verdict->confidence = AXIS_IDENTIFY_UNKNOWN;
	decide_into(verdict, e);
	/* A folder with more videos than are read: those are in no unit.  Its
	 * own unit is not taken as sure (an import's link stands). */
	if (e->not_read > 0 && !verdict->waiting) {
		reason(verdict, "too_many_videos", "the folder holds %u %s more than Axis reads of one folder; %s "
			"not identified", e->not_read, e->not_read == 1 ? "video" : "videos", e->not_read == 1 ? "it is" :
			"they are");
		if (verdict->confidence == AXIS_IDENTIFY_SURE && e->imported == NULL &&
		    e->reading.kind != AXIS_IDENTIFY_READ_ALONE)
			verdict->confidence = AXIS_IDENTIFY_LIKELY;
	}
	/* A series' unit: what its files hold, last (a verdict of these rules
	 * always says it; one that does not was made by an earlier release's). */
	if (e->kind == AXIS_META_KIND_SERIES && e->shape != NULL) {
		g_autofree gchar *words = axis_identify_shape_describe(e->shape);

		reason(verdict, "files", "its files hold %s", words);
	}
	return verdict;
}

static void
decide_into(
	AxisIdentifyVerdict        *verdict,
	const AxisIdentifyEvidence *e
){
	AxisIdentifyConfidence cap = AXIS_IDENTIFY_SURE;
	AxisIdentifyConfidence names;
	gboolean from_id = FALSE;

	/* An import of Axis placed it: that link stands. */
	if (e->imported != NULL) {
		g_autofree gchar *label = label_of(e->imported);

		reason(verdict, "imported", "imported by Axis as %s", label);
		candidate(verdict, e->imported);
		verdict->confidence = AXIS_IDENTIFY_SURE;
		verdict->title = e->imported;
		return;
	}
	if (weigh_id(verdict, &e->name_id, &e->reading, "id_name", FALSE, e->kind, &cap, &from_id))
		return;
	if (e->name_id.state != AXIS_IDENTIFY_ID_FOUND &&
	    weigh_id(verdict, &e->nfo_id, &e->reading, "id_nfo", TRUE, e->kind, &cap, &from_id))
		return;

	/* A full record the decision needs was not answered: nothing can be
	 * told yet, and nothing final is stored. */
	if (e->untold != NULL) {
		g_autofree gchar *label = label_of(e->untold);

		guint i;

		reason(verdict, "provider_waiting", "the details provider did not answer for the full record of %s%s%s%s, "
			"which the decision needs: nothing is decided yet, and Axis asks again later", label,
			e->untold_problem != NULL ? " (" : "", e->untold_problem != NULL ? e->untold_problem : "",
			e->untold_problem != NULL ? ")" : "");
		/* The candidates known so far: every title the names fit, by what is
		 * stored of it (the one awaited among them only when it fits). */
		for (i = 0; e->found != NULL && i < e->n_found; i++) {
			const AxisMetaTitle *record = record_of(e, e->found[i]);

			if (e->found[i] != NULL && (axis_identify_title_fits(e->reading.primary, record) ||
			    axis_identify_title_fits(e->reading.secondary, record)))
				candidate(verdict, record);
		}
		for (i = 0; e->series != NULL && i < e->n_series; i++)
			candidate(verdict, e->series[i].title);
		verdict->confidence = AXIS_IDENTIFY_UNKNOWN;
		verdict->waiting = TRUE;
		return;
	}

	names = weigh_names(verdict, e);
	if (names == AXIS_IDENTIFY_UNKNOWN && from_id)
		names = AXIS_IDENTIFY_LIKELY;
	/* The worse of what the names say and what the identifiers allow. */
	verdict->confidence = MAX(names, cap);
	if (verdict->confidence != AXIS_IDENTIFY_UNKNOWN) {
		if (verdict->candidates->len > 0)
			verdict->title = g_ptr_array_index(verdict->candidates, 0);
		else
			verdict->confidence = AXIS_IDENTIFY_UNKNOWN;
	}
}

/* ── What would settle a unit held for a person ─────────────────── */

static gboolean
has_code(
	const GPtrArray *reasons,
	const gchar     *code
){
	guint i;

	for (i = 0; reasons != NULL && i < reasons->len; i++) {
		const AxisIdentifyReason *r = g_ptr_array_index(reasons, i);

		if (g_strcmp0(r->code, code) == 0)
			return TRUE;
	}
	return FALSE;
}

const gchar *
axis_identify_settle_text(
	AxisMetaKind     kind,
	const GPtrArray *reasons
){
	gboolean series = kind == AXIS_META_KIND_SERIES;

	/* The first that applies, the most decisive cause first. */
	if (has_code(reasons, "provider_waiting"))
		return series ? "The details provider did not answer for a series this folder may be: Axis looks at it again "
			"by itself once it answers, or you can choose the series here now." : "The details provider did not "
			"answer for a movie this folder may be: Axis looks at it again by itself once it answers, or you can "
			"choose the movie here now.";
	if (has_code(reasons, "too_many_videos"))
		return "The folder holds more videos than Axis reads of one folder: split it into one folder per title, or "
			"confirm the title here for the videos it read.";
	if (has_code(reasons, "id_conflict") || has_code(reasons, "id_unknown") || has_code(reasons, "id_disagrees") ||
	    has_code(reasons, "id_uncorroborated"))
		return "The identifier in its name does not settle it: choose the right title here, or correct the "
			"identifier in the name.";
	if (has_code(reasons, "disagree"))
		return "The folder's name and the file's name give different titles: choose the right one here.";
	if (has_code(reasons, "series_like"))
		return "The name looks like part of a series (an episode or a special): search for its own title, which the "
			"search box holds, and choose it, or mark it as not a movie.";
	if (has_code(reasons, "no_title") || has_code(reasons, "no_match"))
		return series ? "Search for the series under another name and choose it, or mark the folder as not a "
			"series." : "Search for the movie under another name and choose it, or mark the folder as not a movie.";
	if (has_code(reasons, "series_ambiguous") || has_code(reasons, "more_series"))
		return "Several series have this name: choose the right one here, or add the year it first aired to the "
			"folder's name, in brackets after the title, and Axis links it by itself.";
	if (has_code(reasons, "series_unchecked"))
		return "Another series of this name cannot be checked against the episodes on disk (the details provider lists no "
			"seasons or episode counts for it): confirm the series here if it is the right one, or choose another.";
	if (has_code(reasons, "ambiguous") || has_code(reasons, "more_titles"))
		return series ? "Several series have this name: choose the right one here (the reasons give each one's year "
			"and runtime), or add the year it first aired to the folder's name, in brackets after the title, and Axis "
			"links it by itself." : "Several movies have this name and the year and the runtime do not tell them apart: "
			"choose the right one here (the reasons give each one's year and runtime), or add its year to the folder's "
			"name, in brackets after the title, if it has none.";
	if (has_code(reasons, "few_episodes"))
		return "Confirm the series here if it is the right one. Axis links it by itself once at least "
			G_STRINGIFY(AXIS_IDENTIFY_SHAPE_MIN_EPISODES) " of its numbered episodes are on disk, or once the "
			"folder's name gives its year.";
	if (has_code(reasons, "season_missing") || has_code(reasons, "episodes_off") ||
	    has_code(reasons, "episodes_unknown") || has_code(reasons, "no_seasons"))
		return "Its episode files do not match the series' seasons, perhaps because they follow another numbering: "
			"confirm the series here if it is the right one, or choose another.";
	if (has_code(reasons, "runtime_off"))
		return series ? "Its files run longer or shorter than the series' episodes: confirm the series here if it is "
			"the right one, or choose another." : "The file runs longer or shorter than the movie: confirm the movie "
			"here if it is the right one (a different cut, perhaps), or choose another.";
	if (has_code(reasons, "no_fit"))
		return "Its episode files fit no series of this name: confirm the series here if it is the right one, or "
			"choose another.";
	if (has_code(reasons, "no_probe"))
		return "Axis looks at it again by itself once its files are read, or you can confirm it here now.";
	if (has_code(reasons, "no_year"))
		return series ? "Confirm the series here if it is the right one, or add the year it first aired to the "
			"folder's name, in brackets after the title, and Axis links it by itself." :
			"Confirm the movie here if it is the right one, or add its year to the folder's name, in brackets after "
			"the title, and Axis links it by itself.";
	if (has_code(reasons, "year_off"))
		return "The year in the name is a year or two off the details provider's: confirm the title here if it is the right "
			"one, or choose another.";
	if (has_code(reasons, "year_unknown"))
		return "The details provider gives no year to check the name's year by: confirm it here if it is the right one.";
	if (has_code(reasons, "no_runtime"))
		return "The details provider gives no runtime to check it by: confirm it here if it is the right one.";
	return "Confirm the best candidate here if it is the right one, or choose another.";
}

/* The reasons against, the most decisive first, as above. */
static const gchar *const doubt_causes[] = {
	"provider_waiting", "too_many_videos", "id_conflict", "id_unknown", "id_disagrees", "id_uncorroborated",
	"disagree", "series_like", "no_title", "no_match", "series_ambiguous", "more_series", "series_unchecked", "ambiguous",
	"more_titles", "other_name_partial", "few_episodes", "season_missing", "episodes_off", "episodes_unknown",
	"no_seasons", "runtime_off", "no_fit", "no_probe", "no_year", "year_off", "year_unknown", "no_runtime", NULL
};

const gchar *
axis_identify_doubt_cause(const GPtrArray *reasons)
{
	guint i;
	guint j;

	for (i = 0; doubt_causes[i] != NULL; i++) {
		for (j = 0; reasons != NULL && j < reasons->len; j++) {
			const AxisIdentifyReason *r = g_ptr_array_index(reasons, j);

			if (g_strcmp0(r->code, doubt_causes[i]) == 0)
				return r->text;
		}
	}
	return NULL;
}

/* ── A series' folder and its episode files ─────────────────────── */

/* Digits only, 1 to 5 of them, no more than 10000: the number; else -1. */
static gint
season_number_of(const gchar *text)
{
	gint value = 0;
	guint n = 0;

	while (*text == ' ')
		text++;
	for (; g_ascii_isdigit(*text); text++) {
		if (++n > 5)
			return -1;
		value = value * 10 + (*text - '0');
	}
	while (*text == ' ')
		text++;
	if (n == 0 || *text != '\0' || value > 10000)
		return -1;
	return value;
}

gint
axis_identify_season_folder(const gchar *name)
{
	g_autofree gchar *trimmed = NULL;
	gsize n;

	if (name == NULL || strlen(name) > 64)
		return -1;
	trimmed = g_strstrip(g_ascii_strdown(name, -1));
	n = strlen(trimmed);
	if (strcmp(trimmed, "specials") == 0 || strcmp(trimmed, "special") == 0)
		return 0;
	if (g_str_has_prefix(trimmed, "season") && n > 6)
		return season_number_of(trimmed + 6);
	if (g_str_has_prefix(trimmed, "series") && n > 6 && trimmed[6] == ' ')
		return season_number_of(trimmed + 6);
	if (trimmed[0] == 's' && n > 1 && g_ascii_isdigit(trimmed[1]))
		return season_number_of(trimmed + 1);
	return -1;
}

void
axis_identify_episodes_clear(AxisIdentifyEpisodes *episodes)
{
	if (episodes == NULL)
		return;
	g_clear_pointer(&episodes->text, g_free);
}

void
axis_identify_episodes_read(
	const AxisRelease    *file,
	gint                  folder_season,
	AxisIdentifyEpisodes *out
){
	AxisReleaseStates states;
	guint i;
	guint k;

	memset(out, 0, sizeof(*out));
	out->season = -1;
	axis_release_states(file, &states);
	switch (states.kind) {
	case AXIS_RELEASE_HOLDS_DATE:
		memcpy(out->date, states.date, sizeof(out->date));
		out->text = g_strdup_printf("dated %s", states.date);
		return;
	case AXIS_RELEASE_HOLDS_COMPLETE:
		out->code = "season_pack";
		out->text = g_strdup("its name says it is the complete series, not an episode");
		return;
	case AXIS_RELEASE_HOLDS_SEASONS:
		out->code = "season_pack";
		out->text = states.season_first == states.season_last ? g_strdup_printf("its name says it is the whole of "
			"season %d, not an episode", states.season_first) : g_strdup_printf("its name says it holds seasons %d "
			"to %d, not an episode", states.season_first, states.season_last);
		return;
	case AXIS_RELEASE_HOLDS_NOTHING:
		out->code = states.code != NULL ? states.code : "no_episode";
		if (g_strcmp0(states.code, "absolute_only") == 0)
			out->text = g_strdup_printf("its name gives %san absolute episode number (%d), which Axis does not "
				"match to a season and an episode yet", states.with_season ? "a season and " : "only ",
				states.absolute);
		else if (g_strcmp0(states.code, "cross_season") == 0)
			out->text = g_strdup("its name numbers episodes across seasons, which is not read");
		else
			out->text = g_strdup("its name gives no season and episode number");
		return;
	default:
		break;
	}
	if (folder_season >= 0 && folder_season != states.season_first) {
		out->code = "season_folder";
		out->text = folder_season == 0 ? g_strdup_printf("its name says season %d, but it is in the specials' "
			"folder", states.season_first) : g_strdup_printf("its name says season %d, but it is in the folder of "
			"season %d", states.season_first, folder_season);
		return;
	}
	out->season = states.season_first;
	for (i = 0; i < states.n_episodes && out->n < AXIS_IDENTIFY_FILE_EPISODES; i++) {
		gboolean seen = FALSE;

		for (k = 0; k < out->n && !seen; k++)
			seen = out->numbers[k] == states.episodes[i];
		if (!seen)
			out->numbers[out->n++] = states.episodes[i];
	}
	{
		GString *text = g_string_new(NULL);
		gboolean run = out->n > 1;

		g_string_append_printf(text, "S%02dE%02d", out->season, out->numbers[0]);
		for (i = 1; i < out->n; i++) {
			if (out->numbers[i] != out->numbers[i - 1] + 1)
				run = FALSE;
		}
		if (run) {
			g_string_append_printf(text, "-E%02d", out->numbers[out->n - 1]);
		} else {
			for (i = 1; i < out->n; i++)
				g_string_append_printf(text, "E%02d", out->numbers[i]);
		}
		out->text = g_string_free(text, FALSE);
	}
}

gint
axis_identify_series_name(
	const AxisIdentifyName *const *files,
	guint                          n
){
	g_autoptr(GHashTable) counts = g_hash_table_new(g_str_hash, g_str_equal);
	g_autoptr(GHashTable) first = g_hash_table_new(g_str_hash, g_str_equal);
	gint best = -1;
	guint best_count = 0;
	guint i;

	/* How many share each title, and where each title is first. */
	for (i = 0; i < n; i++) {
		guint count;

		if (files[i] == NULL || files[i]->key == NULL || !axis_release_is_episode(files[i]->release))
			continue;
		count = GPOINTER_TO_UINT(g_hash_table_lookup(counts, files[i]->key)) + 1;
		g_hash_table_insert(counts, files[i]->key, GUINT_TO_POINTER(count));
		if (!g_hash_table_contains(first, files[i]->key))
			g_hash_table_insert(first, files[i]->key, GUINT_TO_POINTER(i + 1));
	}
	for (i = 0; i < n; i++) {
		guint count;

		if (files[i] == NULL || files[i]->key == NULL || !axis_release_is_episode(files[i]->release) ||
		    GPOINTER_TO_UINT(g_hash_table_lookup(first, files[i]->key)) != i + 1)
			continue;
		count = GPOINTER_TO_UINT(g_hash_table_lookup(counts, files[i]->key));
		if (count > best_count) {
			best = (gint)i;
			best_count = count;
		}
	}
	return best;
}

/* ── A series' episodes on disk against a series' seasons ────────── */

void
axis_identify_shape_init(AxisIdentifyShape *shape)
{
	memset(shape, 0, sizeof(*shape));
	shape->episodes = g_array_new(FALSE, FALSE, sizeof(gint64));
}

void
axis_identify_shape_add(
	AxisIdentifyShape          *shape,
	const AxisIdentifyEpisodes *episodes
){
	guint i;

	if (shape->episodes == NULL)
		shape->episodes = g_array_new(FALSE, FALSE, sizeof(gint64));
	shape->n_files++;
	if (episodes->code != NULL) {
		shape->n_none++;
		return;
	}
	if (episodes->date[0] != '\0') {
		shape->n_dated++;
		return;
	}
	if (episodes->n == 0) {
		shape->n_none++;
		return;
	}
	if (episodes->season <= 0) {
		shape->n_specials++;
		return;
	}
	shape->n_numbered++;
	for (i = 0; i < episodes->n && i < AXIS_IDENTIFY_FILE_EPISODES; i++) {
		gint64 packed;

		if (episodes->numbers[i] < 0)
			continue;
		packed = (gint64)episodes->season * AXIS_IDENTIFY_SHAPE_PACK + episodes->numbers[i];
		g_array_append_val(shape->episodes, packed);
	}
}

static gint
compare_packed(
	gconstpointer a,
	gconstpointer b
){
	gint64 x = *(const gint64 *)a;
	gint64 y = *(const gint64 *)b;

	return x < y ? -1 : x > y ? 1 : 0;
}

void
axis_identify_shape_finish(AxisIdentifyShape *shape)
{
	guint kept = 0;
	guint i;

	if (shape->episodes == NULL || shape->episodes->len == 0)
		return;
	g_array_sort(shape->episodes, compare_packed);
	for (i = 0; i < shape->episodes->len; i++) {
		gint64 value = g_array_index(shape->episodes, gint64, i);

		if (kept > 0 && g_array_index(shape->episodes, gint64, kept - 1) == value)
			continue;
		g_array_index(shape->episodes, gint64, kept) = value;
		kept++;
	}
	g_array_set_size(shape->episodes, kept);
}

void
axis_identify_shape_clear(AxisIdentifyShape *shape)
{
	if (shape->episodes != NULL)
		g_array_unref(shape->episodes);
	memset(shape, 0, sizeof(*shape));
}

gchar *
axis_identify_shape_describe(const AxisIdentifyShape *shape)
{
	GString *text = g_string_new(NULL);
	guint n = shape->episodes != NULL ? shape->episodes->len : 0;

	if (n == 0) {
		g_string_append(text, "no numbered episode");
	} else {
		gint first = (gint)(g_array_index(shape->episodes, gint64, 0) / AXIS_IDENTIFY_SHAPE_PACK);
		gint last = (gint)(g_array_index(shape->episodes, gint64, n - 1) / AXIS_IDENTIFY_SHAPE_PACK);

		g_string_append_printf(text, "%u numbered %s of ", n, n == 1 ? "episode" : "episodes");
		if (first == last)
			g_string_append_printf(text, "season %d", first);
		else
			g_string_append_printf(text, "seasons %d to %d", first, last);
	}
	if (shape->n_specials > 0) {
		g_string_append_printf(text, "; %u %s specials", shape->n_specials, shape->n_specials == 1 ?
			(n > 0 ? "more file is" : "file is") : (n > 0 ? "more files are" : "files are"));
	}
	if (shape->n_dated > 0)
		g_string_append_printf(text, "; %u %s dated, as a daily show's", shape->n_dated, shape->n_dated == 1 ?
			"file is" : "files are");
	if (shape->n_none > 0)
		g_string_append_printf(text, "; %u %s no episode", shape->n_none, shape->n_none == 1 ? "file holds" :
			"files hold");
	return g_string_free(text, FALSE);
}

void
axis_identify_shape_fit(
	const AxisIdentifyShape *shape,
	const AxisMetaTitle     *series,
	AxisIdentifyFit         *fit
){
	guint i;

	memset(fit, 0, sizeof(*fit));
	fit->missing_season = -1;
	fit->off_season = -1;
	fit->off_number = -1;
	if (series == NULL || series->seasons == NULL || series->seasons->len == 0)
		return;
	fit->listed = TRUE;
	for (i = 0; shape->episodes != NULL && i < shape->episodes->len; i++) {
		gint64 packed = g_array_index(shape->episodes, gint64, i);
		gint season = (gint)(packed / AXIS_IDENTIFY_SHAPE_PACK);
		gint number = (gint)(packed % AXIS_IDENTIFY_SHAPE_PACK);
		const AxisMetaSeason *found = NULL;
		guint k;

		for (k = 0; k < series->seasons->len && found == NULL; k++) {
			const AxisMetaSeason *s = g_ptr_array_index(series->seasons, k);

			if (s != NULL && s->number == season)
				found = s;
		}
		if (found == NULL) {
			fit->missing++;
			if (fit->missing_season < 0)
				fit->missing_season = season;
			continue;
		}
		if (found->episode_count < 0) {
			fit->unchecked++;
			continue;
		}
		fit->checked++;
		if (number >= 1 && number <= found->episode_count) {
			fit->landed++;
		} else if (fit->off_season < 0) {
			fit->off_season = season;
			fit->off_number = number;
		}
	}
	fit->fits = fit->missing == 0 && fit->checked >= AXIS_IDENTIFY_SHAPE_MIN_EPISODES &&
		(guint64)fit->landed * 100 >= (guint64)fit->checked * AXIS_IDENTIFY_SHAPE_SHARE;
}

/* ── Quality ────────────────────────────────────────────────────── */

void
axis_identify_quality_clear(AxisIdentifyQuality *quality)
{
	g_clear_pointer(&quality->text, g_free);
}

AxisReleaseResolution
axis_identify_resolution_from_name(const gchar *name)
{
	AxisReleaseResolution r;

	if (name == NULL)
		return AXIS_RELEASE_RESOLUTION_NONE;
	for (r = AXIS_RELEASE_RESOLUTION_480P; r <= AXIS_RELEASE_RESOLUTION_2160P; r++) {
		if (g_strcmp0(axis_release_resolution_name(r), name) == 0)
			return r;
	}
	return AXIS_RELEASE_RESOLUTION_NONE;
}

static AxisReleaseResolution
probe_resolution(const gchar *probe_class)
{
	if (g_strcmp0(probe_class, "sd") == 0)
		return AXIS_RELEASE_RESOLUTION_480P;
	if (g_strcmp0(probe_class, "720p") == 0)
		return AXIS_RELEASE_RESOLUTION_720P;
	if (g_strcmp0(probe_class, "1080p") == 0)
		return AXIS_RELEASE_RESOLUTION_1080P;
	if (g_strcmp0(probe_class, "2160p") == 0)
		return AXIS_RELEASE_RESOLUTION_2160P;
	return AXIS_RELEASE_RESOLUTION_NONE;
}

/* Sources the ladder places by the source alone. */
static gboolean
source_alone(AxisReleaseSource source)
{
	return source == AXIS_RELEASE_SOURCE_DVD || source == AXIS_RELEASE_SOURCE_DISC ||
		(source >= AXIS_RELEASE_SOURCE_CAM && source <= AXIS_RELEASE_SOURCE_WORKPRINT);
}

void
axis_identify_file_quality(
	const AxisRelease   *file,
	const AxisRelease   *folder,
	const gchar         *probe_class,
	AxisIdentifyQuality *out
){
	AxisReleaseSource source = file != NULL ? file->source : AXIS_RELEASE_SOURCE_NONE;
	AxisReleaseResolution resolution = file != NULL ? file->resolution : AXIS_RELEASE_RESOLUTION_NONE;
	const gchar *source_from = "its name";
	const gchar *resolution_from = "its name";

	memset(out, 0, sizeof(*out));
	if (source == AXIS_RELEASE_SOURCE_NONE && folder != NULL && folder->source != AXIS_RELEASE_SOURCE_NONE) {
		source = folder->source;
		source_from = "its folder's name";
	}
	if (resolution == AXIS_RELEASE_RESOLUTION_NONE && folder != NULL &&
	    folder->resolution != AXIS_RELEASE_RESOLUTION_NONE) {
		resolution = folder->resolution;
		resolution_from = "its folder's name";
	}
	if (resolution == AXIS_RELEASE_RESOLUTION_NONE) {
		resolution = probe_resolution(probe_class);
		resolution_from = "the probe";
	}
	if (source != AXIS_RELEASE_SOURCE_NONE && (source_alone(source) || resolution != AXIS_RELEASE_RESOLUTION_NONE))
		out->quality = axis_quality_of(source, source_alone(source) ? AXIS_RELEASE_RESOLUTION_NONE : resolution);
	if (out->quality != AXIS_QUALITY_UNKNOWN) {
		if (source_alone(source) || strcmp(source_from, resolution_from) == 0)
			out->text = g_strdup_printf("%s, from %s", axis_quality_label(out->quality), source_from);
		else
			out->text = g_strdup_printf("%s: the source from %s, %s from %s", axis_quality_label(out->quality),
				source_from, axis_release_resolution_name(resolution), resolution_from);
		return;
	}
	if (resolution != AXIS_RELEASE_RESOLUTION_NONE) {
		out->resolution = resolution;
		out->text = g_strdup_printf("%s: no source is stated (%s from %s)",
			axis_quality_unknown_source_label(resolution), axis_release_resolution_name(resolution), resolution_from);
		return;
	}
	out->text = g_strdup("unknown: no source is stated and no resolution is known");
}
