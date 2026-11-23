/*
 * test-analysis.c - the Analysis reports against a library with known
 * answers (tests/axis-test-analysis.h): space, composition, bit rates,
 * largest, duplicates, split series and albums, leftovers, coverage,
 * snapshots and their retention, and when a refresh is due
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Files are real (small) files in scratch directories, scanned by the real
 * scanner; what the probe found is written by SQL.  Every expected figure
 * below is worked out by hand from the scenario.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <glib.h>
#include <string.h>

#include "axis-test-analysis.h"
#include "axis-test-db.h"
#include "axis-test-jobs.h"
#include "axis-test-library.h"
#include "core/axis-settings.h"
#include "jobs/axis-job-builtin.h"
#include "jobs/axis-job-queue.h"
#include "jobs/axis-job-registry.h"
#include "library/axis-analysis.h"
#include "library/axis-library-browse.h"
#include "library/axis-library-filter.h"

typedef struct
{
	AxisTestLibrary  *t;
	AxisSettings     *settings;
	AxisAnalysis     *analysis;
	AxisTestScenario  s;
} Fixture;

#define SKIP_WITHOUT_DB(fx) do { if ((fx)->t == NULL) return; } while (0)

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;

	memset(fx, 0, sizeof(*fx));
	axis_test_log_init();
	axis_test_log_reset();
	/* Making the scenario takes the first disk away for a moment. */
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*is offline at*");
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*does not match what is at*");

	fx->t = axis_test_library_new(8);
	if (fx->t == NULL)
		return;
	fx->settings = axis_settings_new();
	g_assert_true(axis_settings_add_table(fx->settings, AXIS_ANALYSIS_MODULE_ID, axis_analysis_get_setting_table(),
		&error));
	g_assert_true(axis_settings_load(fx->settings, NULL, &error));
	g_assert_no_error(error);
	fx->analysis = axis_analysis_new(fx->t->pool, fx->settings);
	axis_test_scenario_build(fx->t, &fx->s);
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	if (fx->analysis != NULL)
		axis_analysis_unref(fx->analysis);
	if (fx->settings != NULL)
		axis_settings_unref(fx->settings);
	axis_test_scenario_clear(&fx->s);
	axis_test_library_free(fx->t);
	axis_test_log_reset();
}

/* ── Helpers ────────────────────────────────────────────────────── */

static gint64
refresh(Fixture *fx)
{
	g_autoptr(GError) error = NULL;
	gint64 id = 0;

	if (!axis_analysis_refresh(fx->analysis, "direct", 0, NULL, &id, &error))
		g_error("the refresh failed: %s", error != NULL ? error->message : "?");
	g_assert_cmpint(id, >, 0);
	return id;
}

static AxisAnalysisSnapshot *
load(
	Fixture *fx,
	gint64   id
){
	g_autoptr(GError) error = NULL;
	AxisAnalysisSnapshot *snapshot = NULL;

	g_assert_true(axis_analysis_load_snapshot(fx->analysis, id, NULL, &snapshot, &error));
	g_assert_no_error(error);
	g_assert_nonnull(snapshot);
	return snapshot;
}

static AxisAnalysisSnapshot *
fresh(Fixture *fx)
{
	return load(fx, refresh(fx));
}

static const AxisAnalysisFact *
fact(
	const AxisAnalysisSnapshot *snapshot,
	const gchar                *report,
	const gchar                *dim1,
	const gchar                *dim2
){
	const AxisAnalysisFact *found = axis_analysis_snapshot_fact(snapshot, report, dim1, dim2);

	if (found == NULL)
		g_error("the snapshot has no %s/%s/%s", report, dim1 != NULL ? dim1 : "", dim2 != NULL ? dim2 : "");
	return found;
}

static void
assert_fact(
	const AxisAnalysisSnapshot *snapshot,
	const gchar                *report,
	const gchar                *dim1,
	const gchar                *dim2,
	gint64                      paths,
	gint64                      bytes
){
	const AxisAnalysisFact *found = fact(snapshot, report, dim1, dim2);

	if (found->paths != paths || found->bytes != bytes)
		g_error("%s/%s/%s: expected %" G_GINT64_FORMAT " paths and %" G_GINT64_FORMAT " bytes, got %"
			G_GINT64_FORMAT " and %" G_GINT64_FORMAT, report, dim1 != NULL ? dim1 : "",
			dim2 != NULL ? dim2 : "", paths, bytes, found->paths, found->bytes);
}

static gchar *
id_text(gint64 id)
{
	return g_strdup_printf("%" G_GINT64_FORMAT, id);
}

static JsonArray *
items(const AxisAnalysisFact *found)
{
	g_assert_true(json_object_has_member(found->extra, "items"));
	return json_object_get_array_member(found->extra, "items");
}

/* How many files the file list finds for a query: a figure's link. */
static gint64
list_count(
	Fixture     *fx,
	const gchar *query
){
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisFileList) list = NULL;
	AxisLibraryFilter filter;

	if (!axis_library_filter_parse(query, &filter, &error))
		g_error("the list refuses '%s': %s", query, error->message);
	g_assert_true(axis_library_list_files(fx->t->library, &filter, &list, &error));
	g_assert_no_error(error);
	g_assert_false(list->count_capped);
	g_assert_false(list->count_unknown);
	axis_library_filter_clear(&filter);
	return (gint64)list->count;
}

static gint
compare_names(
	gconstpointer a,
	gconstpointer b
){
	return g_ascii_strcasecmp(*(gchar *const *)a, *(gchar *const *)b);
}

/* The base names of the files a query lists, sorted, joined with "|". */
static gchar *
list_names(
	Fixture     *fx,
	const gchar *query
){
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisFileList) list = NULL;
	g_autoptr(GPtrArray) names = g_ptr_array_new_with_free_func(g_free);
	AxisLibraryFilter filter;
	guint i;

	g_assert_true(axis_library_filter_parse(query, &filter, &error));
	g_assert_true(axis_library_list_files(fx->t->library, &filter, &list, &error));
	for (i = 0; i < list->files->len; i++) {
		const AxisBrowseFile *file = g_ptr_array_index(list->files, i);
		const gchar *slash = strrchr(file->rel_path, '/');

		g_ptr_array_add(names, g_strdup(slash != NULL ? slash + 1 : file->rel_path));
	}
	g_ptr_array_sort(names, compare_names);
	g_ptr_array_add(names, NULL);
	axis_library_filter_clear(&filter);
	return g_strjoinv("|", (gchar **)names->pdata);
}

/* ── Space ──────────────────────────────────────────────────────── */

/* Spec: Totals agree with the index; Hardlinks counted once. */
static void
test_space(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisAnalysisSnapshot) snapshot = NULL;
	g_autofree gchar *a = NULL;
	g_autofree gchar *b = NULL;
	g_autofree gchar *independent = NULL;
	const GPtrArray *facts;
	const AxisAnalysisFact *overall;
	gint64 root_bytes = 0;
	gint64 root_paths = 0;
	guint i;

	SKIP_WITHOUT_DB(fx);
	snapshot = fresh(fx);
	a = id_text(fx->s.vol_a);
	b = id_text(fx->s.vol_b);

	overall = fact(snapshot, "overall", "", NULL);
	g_assert_cmpint(overall->paths, ==, AXIS_TEST_SCENARIO_PATHS);
	g_assert_cmpint(overall->bytes, ==, AXIS_TEST_SCENARIO_BYTES);
	g_assert_cmpint(axis_analysis_extra_int(overall->extra, "linked_paths"), ==, 2);
	g_assert_cmpint(axis_analysis_extra_int(overall->extra, "offline_paths"), ==, 0);
	g_assert_cmpint(snapshot->paths, ==, AXIS_TEST_SCENARIO_PATHS);
	g_assert_cmpint(snapshot->bytes, ==, AXIS_TEST_SCENARIO_BYTES);

	/* The same total, from the index by another route: one row per file
	 * (volume and inode). */
	independent = axis_test_library_q(fx->t, "SELECT sum(size) FROM (SELECT DISTINCT ON (volume_id, inode) size"
		" FROM library_file WHERE state = 'present' ORDER BY volume_id, inode, id) x");
	g_assert_cmpint(g_ascii_strtoll(independent, NULL, 10), ==, overall->bytes);

	/* By volume: the hard links of A count once, in every total. */
	assert_fact(snapshot, "space_volume", a, NULL, AXIS_TEST_SCENARIO_PATHS_A, AXIS_TEST_SCENARIO_BYTES_A);
	assert_fact(snapshot, "space_volume", b, NULL, AXIS_TEST_SCENARIO_PATHS_B, AXIS_TEST_SCENARIO_BYTES_B);
	g_assert_cmpint(axis_analysis_extra_int(fact(snapshot, "space_volume", a, NULL)->extra, "linked_paths"), ==, 2);
	g_assert_cmpint(axis_analysis_extra_int(fact(snapshot, "space_volume", b, NULL)->extra, "linked_paths"), ==, 0);
	g_assert_cmpstr(axis_analysis_extra_string(fact(snapshot, "space_volume", a, NULL)->extra, "name"), ==, "Disk A");
	g_assert_cmpint(axis_analysis_extra_int(fact(snapshot, "space_volume", a, NULL)->extra, "usable"), ==, 1);

	/* By root: the hard link between two roots of one volume is counted
	 * once, in the root whose path was indexed first; the sum of the roots
	 * is the volume's. */
	{
		g_autofree gchar *films = id_text(fx->s.films_a);
		g_autofree gchar *down = id_text(fx->s.down_a);

		assert_fact(snapshot, "space_root", films, NULL, 17, 9875);
		assert_fact(snapshot, "space_root", down, NULL, 1, 0);
		g_assert_cmpint(axis_analysis_extra_int(fact(snapshot, "space_root", down, NULL)->extra, "linked_paths"), ==, 1);
	}
	facts = axis_analysis_snapshot_facts(snapshot, "space_root");
	g_assert_cmpuint(facts->len, ==, 7);
	for (i = 0; i < facts->len; i++) {
		const AxisAnalysisFact *root = g_ptr_array_index((GPtrArray *)facts, i);

		root_bytes += root->bytes;
		root_paths += root->paths;
	}
	g_assert_cmpint(root_bytes, ==, AXIS_TEST_SCENARIO_BYTES);
	g_assert_cmpint(root_paths, ==, AXIS_TEST_SCENARIO_PATHS);

	/* By category, from the root's type. */
	assert_fact(snapshot, "space_type", "movies", NULL, 21, 14662);
	assert_fact(snapshot, "space_type", "tv", NULL, 6, 1660);
	assert_fact(snapshot, "space_type", "music", NULL, 4, 680);
	assert_fact(snapshot, "space_type", "downloads", NULL, 1, 0);
	assert_fact(snapshot, "space_kind", "video", NULL, 25, 16295);
	assert_fact(snapshot, "space_kind", "audio", NULL, 4, 680);
	assert_fact(snapshot, "space_kind", "subtitle", NULL, 2, 22);
	assert_fact(snapshot, "space_kind", "other", NULL, 1, 5);

	/* A snapshot says when it was taken and what it counts. */
	g_assert_cmpint(snapshot->taken_us, >, 0);
	g_assert_cmpstr(snapshot->trigger, ==, "direct");
}

/* Spec: every figure's link lists as many files as the figure counts. */
static void
test_figures_match_lists(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisAnalysisSnapshot) snapshot = NULL;
	g_autofree gchar *a = NULL;
	g_autofree gchar *films = NULL;
	g_autofree gchar *query = NULL;

	SKIP_WITHOUT_DB(fx);
	snapshot = fresh(fx);

	a = id_text(fx->s.vol_a);
	films = id_text(fx->s.films_a);
	query = g_strdup_printf("volume=%s", a);
	g_assert_cmpint(list_count(fx, query), ==, fact(snapshot, "space_volume", a, NULL)->paths);
	g_clear_pointer(&query, g_free);
	query = g_strdup_printf("root=%s", films);
	g_assert_cmpint(list_count(fx, query), ==, fact(snapshot, "space_root", films, NULL)->paths);
	g_assert_cmpint(list_count(fx, "type=movies"), ==, fact(snapshot, "space_type", "movies", NULL)->paths);
	g_assert_cmpint(list_count(fx, "type=downloads"), ==, fact(snapshot, "space_type", "downloads", NULL)->paths);
	g_assert_cmpint(list_count(fx, "kind=video"), ==, fact(snapshot, "space_kind", "video", NULL)->paths);
	g_assert_cmpint(list_count(fx, ""), ==, fact(snapshot, "overall", "", NULL)->paths);
	g_assert_cmpint(list_count(fx, "vcodec=h264"), ==, fact(snapshot, "comp_vcodec", "h264", NULL)->paths);
	g_assert_cmpint(list_count(fx, "res=1080p"), ==, fact(snapshot, "comp_res", "1080p", NULL)->paths);
	g_assert_cmpint(list_count(fx, "hdr=sdr"), ==, fact(snapshot, "comp_hdr", "sdr", NULL)->paths);
	g_assert_cmpint(list_count(fx, "container=flac"), ==, fact(snapshot, "comp_container", "flac", NULL)->paths);
	g_assert_cmpint(list_count(fx, "acodec=aac"), ==, fact(snapshot, "comp_acodec", "aac", NULL)->paths);
}

/* Spec: Hardlinks are not duplicates. */
static void
test_hardlinks_are_not_duplicates(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisAnalysisSnapshot) snapshot = NULL;
	static const gchar *const reports[] = { "dup_content", "dup_title", NULL };
	guint r;

	SKIP_WITHOUT_DB(fx);
	snapshot = fresh(fx);

	for (r = 0; reports[r] != NULL; r++) {
		JsonArray *groups = items(fact(snapshot, reports[r], "", NULL));
		guint i;

		for (i = 0; i < json_array_get_length(groups); i++) {
			JsonObject *group = json_array_get_object_element(groups, i);
			JsonArray *members = json_object_get_array_member(group, "members");
			g_autoptr(GString) list = g_string_new(NULL);
			g_autofree gchar *twice = NULL;
			guint k;

			for (k = 0; k < json_array_get_length(members); k++)
				g_string_append_printf(list, "%s%" G_GINT64_FORMAT, k > 0 ? "," : "",
					json_object_get_int_member(json_array_get_object_element(members, k), "id"));
			/* No two members are paths of one file. */
			twice = axis_test_library_q(fx->t, "SELECT count(*) FROM (SELECT 1 FROM library_file WHERE id IN (%s)"
				" GROUP BY volume_id, inode HAVING count(*) > 1) x", list->str);
			g_assert_cmpstr(twice, ==, "0");
		}
	}

	/* The Hearth of 1995 is three paths of one file: no group of copies. */
	{
		JsonArray *groups = items(fact(snapshot, "dup_title", "", NULL));
		guint i;

		for (i = 0; i < json_array_get_length(groups); i++) {
			JsonObject *group = json_array_get_object_element(groups, i);
			JsonArray *parts = json_object_get_array_member(group, "parts");

			g_assert_cmpstr(json_array_get_string_element(parts, 0), !=, "hearth");
		}
	}
}

/* ── Composition ────────────────────────────────────────────────── */

/* Spec: Breakdown. */
static void
test_composition(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisAnalysisSnapshot) snapshot = NULL;

	SKIP_WITHOUT_DB(fx);
	snapshot = fresh(fx);

	assert_fact(snapshot, "comp_vcodec", "h264", NULL, 17, 8334);
	assert_fact(snapshot, "comp_vcodec", "hevc", NULL, 2, 7000);
	g_assert_cmpuint(axis_analysis_snapshot_facts(snapshot, "comp_vcodec")->len, ==, 2);
	assert_fact(snapshot, "comp_res", "2160p", NULL, 2, 7000);
	assert_fact(snapshot, "comp_res", "1080p", NULL, 14, 7084);
	assert_fact(snapshot, "comp_res", "720p", NULL, 2, 650);
	assert_fact(snapshot, "comp_res", "sd", NULL, 1, 600);
	assert_fact(snapshot, "comp_hdr", "hdr10", NULL, 1, 4000);
	assert_fact(snapshot, "comp_hdr", "sdr", NULL, 18, 11334);
	assert_fact(snapshot, "comp_container", "matroska,webm", NULL, 19, 15334);
	assert_fact(snapshot, "comp_container", "flac", NULL, 3, 630);
	assert_fact(snapshot, "comp_container", "mp3", NULL, 1, 50);
	assert_fact(snapshot, "comp_acodec", "eac3", NULL, 1, 4000);
	assert_fact(snapshot, "comp_acodec", "aac", NULL, 18, 11334);
	assert_fact(snapshot, "comp_acodec", "flac", NULL, 3, 630);
	assert_fact(snapshot, "comp_acodec", "mp3", NULL, 1, 50);
	/* The hard-linked Hearth is three paths in the count and one in the size. */
	g_assert_cmpint(axis_analysis_extra_int(fact(snapshot, "comp_vcodec", "h264", NULL)->extra, "linked_paths"), ==, 2);
}

/* Spec: Outliers (the threshold per codec, as a setting). */
static void
test_outliers(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisAnalysisSnapshot) snapshot = NULL;
	g_autoptr(AxisAnalysisSnapshot) again = NULL;
	g_autoptr(GError) error = NULL;
	const AxisAnalysisFact *list;
	JsonArray *listed;
	JsonObject *config;

	SKIP_WITHOUT_DB(fx);
	snapshot = fresh(fx);

	/* The defaults: 0.30 for H.264 and 0.15 for HEVC. */
	config = json_object_get_object_member(snapshot->config, "large_bpp_milli");
	g_assert_cmpint(json_object_get_int_member(config, "h264"), ==, 300);
	g_assert_cmpint(json_object_get_int_member(config, "hevc"), ==, 150);
	assert_fact(snapshot, "large", "1080p", "h264", 3, 1000);
	assert_fact(snapshot, "large", "2160p", "hevc", 1, 4000);
	g_assert_cmpuint(axis_analysis_snapshot_facts(snapshot, "large")->len, ==, 2);
	g_assert_cmpint(axis_analysis_extra_int(fact(snapshot, "large", "1080p", "h264")->extra, "threshold_milli"), ==, 300);
	/* The list: the biggest first, one entry for the hard-linked file. */
	list = fact(snapshot, "large_list", "", NULL);
	g_assert_cmpint(list->paths, ==, 2);
	listed = items(list);
	g_assert_cmpuint(json_array_get_length(listed), ==, 2);
	g_assert_cmpint(json_object_get_int_member(json_array_get_object_element(listed, 0), "size"), ==, 4000);
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(listed, 0), "codec"), ==, "hevc");
	g_assert_cmpint(json_object_get_int_member(json_array_get_object_element(listed, 1), "size"), ==, 1000);
	/* A figure links to the same files. */
	g_assert_cmpint(list_count(fx, "vcodec=h264&res=1080p&min_bpp=300"), ==, 3);
	g_assert_cmpint(list_count(fx, "vcodec=hevc&res=2160p&min_bpp=150"), ==, 1);

	/* The distribution: buckets that add up, each as long as its list. */
	{
		const AxisAnalysisFact *cell = fact(snapshot, "bpp", "1080p", "h264");
		JsonArray *buckets = json_object_get_array_member(cell->extra, "buckets");
		gint64 sum = 0;
		guint i;

		g_assert_cmpint(cell->paths, ==, 14);
		for (i = 0; i < json_array_get_length(buckets); i++) {
			JsonArray *entry = json_array_get_array_element(buckets, i);
			gint64 k = json_array_get_int_element(entry, 0);
			gint64 n = json_array_get_int_element(entry, 1);
			g_autofree gchar *query = k < 10 ?
				g_strdup_printf("vcodec=h264&res=1080p&min_bpp=%" G_GINT64_FORMAT "&max_bpp=%" G_GINT64_FORMAT,
					k * 50, (k + 1) * 50) :
				g_strdup_printf("vcodec=h264&res=1080p&min_bpp=%" G_GINT64_FORMAT, k * 50);

			sum += n;
			g_assert_cmpint(list_count(fx, query), ==, n);
		}
		g_assert_cmpint(sum, ==, cell->paths);
		g_assert_cmpfloat_with_epsilon(json_object_get_double_member(cell->extra, "median"), 0.10, 0.0001);
	}

	/* The threshold is a setting: lower it and more files are large. */
	g_assert_true(axis_settings_set(fx->settings, AXIS_ANALYSIS_SETTING_LARGE_H264, "0.11", &error));
	g_assert_no_error(error);
	again = fresh(fx);
	g_assert_cmpint(json_object_get_int_member(json_object_get_object_member(again->config, "large_bpp_milli"),
		"h264"), ==, 110);
	assert_fact(again, "large", "1080p", "h264", 6, 1000 + 650 + 660 + 610);
	assert_fact(again, "large", "sd", "h264", 1, 600);
	g_assert_cmpint(list_count(fx, "vcodec=h264&res=1080p&min_bpp=110"), ==, 6);
	/* The snapshot taken before keeps the threshold it used. */
	g_assert_cmpint(json_object_get_int_member(json_object_get_object_member(snapshot->config, "large_bpp_milli"),
		"h264"), ==, 300);
}

/* ── Largest ────────────────────────────────────────────────────── */

/* Spec: Largest files. */
static void
test_largest(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisAnalysisSnapshot) snapshot = NULL;
	const AxisAnalysisFact *files;
	const AxisAnalysisFact *dirs;
	JsonArray *listed;
	gint64 last = G_MAXINT64;
	guint i;

	SKIP_WITHOUT_DB(fx);
	snapshot = fresh(fx);

	files = fact(snapshot, "largest_files", "", NULL);
	/* One entry for each file, so the three paths of Hearth are one. */
	g_assert_cmpint(files->paths, ==, 30);
	listed = items(files);
	g_assert_cmpuint(json_array_get_length(listed), ==, 30);
	for (i = 0; i < json_array_get_length(listed); i++) {
		JsonObject *item = json_array_get_object_element(listed, i);
		gint64 size = json_object_get_int_member(item, "size");

		g_assert_cmpint(size, <=, last);
		last = size;
		g_assert_nonnull(json_object_get_string_member(item, "path"));
	}
	g_assert_cmpint(json_object_get_int_member(json_array_get_object_element(listed, 0), "size"), ==, 4000);
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(listed, 0), "path"), ==,
		"Alder (1979)/Alder (1979) 2160p.mkv");
	g_assert_cmpint(json_object_get_int_member(json_array_get_object_element(listed, 1), "size"), ==, 3000);
	g_assert_cmpint(json_object_get_int_member(json_array_get_object_element(listed, 2), "size"), ==, 1100);
	g_assert_cmpint(json_object_get_int_member(json_array_get_object_element(listed, 3), "size"), ==, 1000);
	g_assert_cmpint(json_object_get_int_member(json_array_get_object_element(listed, 3), "nlink"), ==, 3);

	/* The folders below the roots' tops, with the Library pages' totals. */
	dirs = fact(snapshot, "largest_dirs", "", NULL);
	g_assert_cmpint(dirs->paths, ==, 17);
	listed = items(dirs);
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(listed, 0), "name"), ==,
		"Alder (1979)");
	g_assert_cmpint(json_object_get_int_member(json_array_get_object_element(listed, 0), "bytes"), ==, 4090);
	g_assert_cmpint(json_object_get_int_member(json_array_get_object_element(listed, 0), "files"), ==, 4);
	{
		g_autofree gchar *total = axis_test_library_q(fx->t, "SELECT total_bytes FROM library_dir WHERE id = %"
			G_GINT64_FORMAT, json_object_get_int_member(json_array_get_object_element(listed, 0), "id"));

		g_assert_cmpstr(total, ==, "4090");
	}
}

/* ── Duplicates ─────────────────────────────────────────────────── */

static JsonObject *
group_with(
	JsonArray   *groups,
	const gchar *kind,
	const gchar *first_part
){
	guint i;

	for (i = 0; i < json_array_get_length(groups); i++) {
		JsonObject *group = json_array_get_object_element(groups, i);
		JsonArray *parts = json_object_get_array_member(group, "parts");

		if (g_strcmp0(json_object_get_string_member(group, "kind"), kind) == 0 &&
		    g_strcmp0(json_array_get_string_element(parts, 0), first_part) == 0)
			return group;
	}
	return NULL;
}

static gint64
member_size(
	JsonObject *group,
	guint       index
){
	return json_object_get_int_member(json_array_get_object_element(
		json_object_get_array_member(group, "members"), index), "size");
}

/* Spec: Same content. */
static void
test_duplicates_content(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisAnalysisSnapshot) snapshot = NULL;
	const AxisAnalysisFact *found;
	JsonArray *groups;
	JsonObject *group;
	JsonArray *members;

	SKIP_WITHOUT_DB(fx);
	snapshot = fresh(fx);

	found = fact(snapshot, "dup_content", "", NULL);
	/* One group: the Brannock of 1985 on both disks.  The hard-linked Hearth
	 * and the files with the same size but not the same length are not. */
	g_assert_cmpint(found->paths, ==, 1);
	g_assert_cmpint(found->bytes, ==, 777);
	groups = items(found);
	g_assert_cmpuint(json_array_get_length(groups), ==, 1);
	group = json_array_get_object_element(groups, 0);
	g_assert_cmpint(json_object_get_int_member(group, "size"), ==, 777);
	g_assert_cmpint(json_object_get_int_member(group, "copies"), ==, 2);
	g_assert_cmpint(json_object_get_int_member(group, "duration_ms"), ==, 8000000);
	g_assert_cmpint(json_object_get_int_member(group, "wasted"), ==, 777);
	/* The evidence: the streams. */
	g_assert_nonnull(strstr(json_object_get_string_member(group, "layout"), "video:h264:1920:1080"));
	members = json_object_get_array_member(group, "members");
	g_assert_cmpuint(json_array_get_length(members), ==, 2);
	{
		JsonObject *first = json_array_get_object_element(members, 0);
		JsonObject *second = json_array_get_object_element(members, 1);

		g_assert_cmpint(json_object_get_int_member(first, "volume_id"), !=, json_object_get_int_member(second, "volume_id"));
		g_assert_cmpstr(json_object_get_string_member(first, "path"), ==, "Brannock (1985)/Brannock (1985).mkv");
		/* What each copy is, for the page. */
		g_assert_cmpint(json_object_get_int_member(first, "size"), ==, 777);
		g_assert_cmpint(json_object_get_int_member(second, "size"), ==, 777);
		g_assert_true(json_object_get_boolean_member(first, "probed"));
		g_assert_cmpstr(json_object_get_string_member(second, "res"), ==, "1080p");
	}
}

/* Spec: Same title, different quality; Different titles stay apart. */
static void
test_duplicates_title(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisAnalysisSnapshot) snapshot = NULL;
	const AxisAnalysisFact *found;
	JsonArray *groups;
	JsonObject *group;
	JsonObject *considered;

	SKIP_WITHOUT_DB(fx);
	snapshot = fresh(fx);

	found = fact(snapshot, "dup_title", "", NULL);
	groups = items(found);
	/* Dunmore, Brannock, one episode and one track: nothing else. */
	g_assert_cmpint(found->paths, ==, 4);
	g_assert_cmpuint(json_array_get_length(groups), ==, 4);
	g_assert_cmpint(found->bytes, ==, 777 + 500 + 150 + 50);

	/* Dunmore: the 2160p copy first, the 720p copy's size is what is
	 * reclaimable. */
	group = group_with(groups, "film", "dunmore");
	g_assert_nonnull(group);
	g_assert_cmpint(json_object_get_int_member(group, "copies"), ==, 2);
	g_assert_cmpint(member_size(group, 0), ==, 3000);
	g_assert_cmpint(member_size(group, 1), ==, 500);
	g_assert_cmpint(json_object_get_int_member(group, "reclaimable"), ==, 500);
	g_assert_cmpstr(json_array_get_string_element(json_object_get_array_member(group, "parts"), 1), ==, "2021");
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(
		json_object_get_array_member(group, "members"), 0), "res"), ==, "2160p");

	group = group_with(groups, "film", "brannock");
	g_assert_nonnull(group);
	g_assert_cmpint(json_object_get_int_member(group, "reclaimable"), ==, 777);

	/* An episode on both disks, in two qualities. */
	group = group_with(groups, "episode", "show x");
	g_assert_nonnull(group);
	g_assert_cmpint(member_size(group, 0), ==, 300);
	g_assert_cmpint(member_size(group, 1), ==, 150);

	/* A track, lossless first. */
	group = group_with(groups, "track", "artist one");
	g_assert_nonnull(group);
	g_assert_cmpint(member_size(group, 0), ==, 200);
	g_assert_cmpint(member_size(group, 1), ==, 50);

	/* Not grouped: films of one title and different years, a title that
	 * contains another, a name without a year, different episodes. */
	g_assert_null(group_with(groups, "film", "hearth"));
	g_assert_null(group_with(groups, "film", "total rewind"));
	g_assert_null(group_with(groups, "film", "the mazework"));
	g_assert_null(group_with(groups, "film", "unknown film"));
	g_assert_null(group_with(groups, "episode", "show y"));
	/* The sample and the extra are not compared. */
	g_assert_null(group_with(groups, "film", "alder"));

	/* What the report read of the names, so that it says what it did not. */
	considered = json_object_get_object_member(found->extra, "considered");
	g_assert_cmpint(json_object_get_int_member(considered, "film"), ==, 11);
	g_assert_cmpint(json_object_get_int_member(considered, "episode"), ==, 6);
	g_assert_cmpint(json_object_get_int_member(considered, "track"), ==, 4);
	considered = json_object_get_object_member(found->extra, "candidates");
	g_assert_cmpint(json_object_get_int_member(considered, "film"), ==, 17);
	g_assert_cmpint(json_object_get_int_member(considered, "episode"), ==, 6);
	g_assert_cmpint(json_object_get_int_member(considered, "track"), ==, 4);
	g_assert_false(json_object_get_boolean_member(found->extra, "truncated"));
}

/* A group's figure is the length of the list it links to. */
static void
test_duplicate_links(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisAnalysisSnapshot) snapshot = NULL;
	JsonArray *groups;
	guint i;

	SKIP_WITHOUT_DB(fx);
	snapshot = fresh(fx);

	groups = items(fact(snapshot, "dup_title", "", NULL));
	for (i = 0; i < json_array_get_length(groups); i++) {
		JsonObject *group = json_array_get_object_element(groups, i);
		JsonArray *members = json_object_get_array_member(group, "members");
		g_autoptr(GString) query = g_string_new("ids=");
		guint k;

		for (k = 0; k < json_array_get_length(members); k++)
			g_string_append_printf(query, "%s%" G_GINT64_FORMAT, k > 0 ? "," : "",
				json_object_get_int_member(json_array_get_object_element(members, k), "id"));
		g_assert_cmpint(list_count(fx, query->str), ==, json_object_get_int_member(group, "copies"));
	}
}

/* ── Split ──────────────────────────────────────────────────────── */

/* Spec: Split series (and albums). */
static void
test_split(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisAnalysisSnapshot) snapshot = NULL;
	const AxisAnalysisFact *series;
	const AxisAnalysisFact *albums;
	JsonObject *item;
	JsonArray *parts;
	guint i;

	SKIP_WITHOUT_DB(fx);
	snapshot = fresh(fx);

	series = fact(snapshot, "split", "series", NULL);
	g_assert_cmpint(series->paths, ==, 1);
	g_assert_cmpint(series->bytes, ==, 610 + 800);
	item = json_array_get_object_element(items(series), 0);
	g_assert_cmpstr(json_object_get_string_member(item, "name"), ==, "Show X (2010)");
	g_assert_cmpint(json_object_get_int_member(item, "volumes"), ==, 2);
	g_assert_cmpint(json_object_get_int_member(item, "moved"), ==, 610);
	parts = json_object_get_array_member(item, "parts");
	g_assert_cmpuint(json_array_get_length(parts), ==, 2);
	/* The bytes on each volume, the larger first. */
	g_assert_cmpint(json_object_get_int_member(json_array_get_object_element(parts, 0), "volume_id"), ==, fx->s.vol_b);
	g_assert_cmpint(json_object_get_int_member(json_array_get_object_element(parts, 0), "bytes"), ==, 800);
	g_assert_cmpint(json_object_get_int_member(json_array_get_object_element(parts, 0), "paths"), ==, 3);
	g_assert_cmpint(json_object_get_int_member(json_array_get_object_element(parts, 1), "volume_id"), ==, fx->s.vol_a);
	g_assert_cmpint(json_object_get_int_member(json_array_get_object_element(parts, 1), "bytes"), ==, 610);
	g_assert_cmpint(json_object_get_int_member(json_array_get_object_element(parts, 1), "paths"), ==, 2);

	albums = fact(snapshot, "split", "album", NULL);
	g_assert_cmpint(albums->paths, ==, 1);
	item = json_array_get_object_element(items(albums), 0);
	g_assert_cmpstr(json_object_get_string_member(item, "name"), ==, "Artist One / Album One");
	g_assert_cmpint(json_object_get_int_member(item, "bytes"), ==, 460);
	g_assert_cmpint(json_object_get_int_member(item, "moved"), ==, 50);

	/* Each part's figure is the length of the list of its folder's files. */
	for (i = 0; i < json_array_get_length(parts); i++) {
		JsonObject *part = json_array_get_object_element(parts, i);
		g_autofree gchar *query = g_strdup_printf("under=%" G_GINT64_FORMAT,
			json_object_get_int_member(part, "dir_id"));

		g_assert_cmpint(json_object_get_int_member(part, "dir_id"), >, 0);
		g_assert_cmpint(list_count(fx, query), ==, json_object_get_int_member(part, "paths"));
	}
	parts = json_object_get_array_member(item, "parts");
	for (i = 0; i < json_array_get_length(parts); i++) {
		JsonObject *part = json_array_get_object_element(parts, i);
		g_autofree gchar *query = g_strdup_printf("under=%" G_GINT64_FORMAT,
			json_object_get_int_member(part, "dir_id"));

		g_assert_cmpint(list_count(fx, query), ==, json_object_get_int_member(part, "paths"));
	}
}

/* ── Leftovers ──────────────────────────────────────────────────── */

/* Spec: Orphaned subtitle; Subtitle beside its video. */
static void
test_leftovers(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisAnalysisSnapshot) snapshot = NULL;
	g_autofree gchar *names = NULL;

	SKIP_WITHOUT_DB(fx);
	snapshot = fresh(fx);

	assert_fact(snapshot, "leftover", "sample", NULL, 1, 50);
	assert_fact(snapshot, "leftover", "extras", NULL, 1, 30);
	assert_fact(snapshot, "leftover", "subtitle", NULL, 1, 12);
	assert_fact(snapshot, "leftover", "junk", NULL, 1, 5);
	assert_fact(snapshot, "leftover", "any", NULL, 4, 97);

	/* The orphan is the subtitle with no video of its name; the one beside
	 * its video is not listed.  Example.mkv and Sampler.mkv are not
	 * samples. */
	names = list_names(fx, "leftover=subtitle");
	g_assert_cmpstr(names, ==, "Other Movie.srt");
	g_clear_pointer(&names, g_free);
	names = list_names(fx, "leftover=sample");
	g_assert_cmpstr(names, ==, "alder-sample.mkv");
	g_clear_pointer(&names, g_free);
	names = list_names(fx, "leftover=extras");
	g_assert_cmpstr(names, ==, "making-of.mp4");
	g_clear_pointer(&names, g_free);
	names = list_names(fx, "leftover=junk");
	g_assert_cmpstr(names, ==, "Thumbs.db");
	g_clear_pointer(&names, g_free);
	g_assert_cmpint(list_count(fx, "leftover=any"), ==, 4);

	/* The biggest of a rule, listed. */
	{
		JsonArray *listed = items(fact(snapshot, "leftover", "sample", NULL));

		g_assert_cmpuint(json_array_get_length(listed), ==, 1);
		g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(listed, 0), "path"), ==,
			"Alder (1979)/Sample/alder-sample.mkv");
	}
}

/* Rows made by SQL, one per case, with the root type they are in. */
static void
insert_file(
	Fixture     *fx,
	gint64       root,
	const gchar *rel,
	const gchar *kind,
	gint64       size,
	gint64       inode
){
	g_autofree gchar *sql = g_strdup_printf(
		"INSERT INTO library_file (root_id, volume_id, rel_path, kind, size, mtime_sec, mtime_nsec, ctime_sec,"
		" ctime_nsec, inode, nlink, pinned, pinned_links, pinned_downloads, state, first_seen_generation,"
		" last_seen_generation, changed_generation, changed)"
		" SELECT %" G_GINT64_FORMAT ", volume_id, '%s', '%s', %" G_GINT64_FORMAT ", 1600000000, 0, 1600000000, 0,"
		" %" G_GINT64_FORMAT ", 1, false, false, false, 'present', 1, 1, 1, false FROM library_root WHERE id = %"
		G_GINT64_FORMAT, root, rel, kind, size, inode, root);

	axis_test_db_exec(fx->t->db, sql);
}

/* A film imported by hard link from a download is a film's size, not a
 * download's, whichever path was indexed first. */
static void
test_hardlink_attribution(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisAnalysisSnapshot) snapshot = NULL;
	g_autofree gchar *films = NULL;
	g_autofree gchar *down = NULL;

	SKIP_WITHOUT_DB(fx);
	axis_test_db_exec(fx->t->db, "DELETE FROM library_file");
	/* The download is indexed first, so it has the lower id. */
	insert_file(fx, fx->s.down_a, "Film.1995.mkv", "video", 1000, 777000);
	insert_file(fx, fx->s.films_a, "Film (1995)/Film (1995).mkv", "video", 1000, 777000);
	axis_test_db_exec(fx->t->db, "UPDATE library_file SET nlink = 2, pinned = true, pinned_links = true"
		" WHERE inode = 777000");
	/* The same inode on the other volume is another file. */
	insert_file(fx, fx->s.films_b, "Film (1995)/Film (1995).mkv", "video", 1000, 777000);
	axis_test_db_exec(fx->t->db, "UPDATE library_file SET nlink = 2, pinned = true, pinned_links = true"
		" WHERE root_id = " "(SELECT id FROM library_root WHERE name = 'Films2')");

	snapshot = fresh(fx);
	films = id_text(fx->s.films_a);
	down = id_text(fx->s.down_a);
	assert_fact(snapshot, "space_root", films, NULL, 1, 1000);
	assert_fact(snapshot, "space_root", down, NULL, 1, 0);
	assert_fact(snapshot, "space_type", "downloads", NULL, 1, 0);
	assert_fact(snapshot, "space_type", "movies", NULL, 2, 2000);
	assert_fact(snapshot, "overall", "", NULL, 3, 2000);
	g_assert_cmpint(axis_analysis_extra_int(fact(snapshot, "overall", "", NULL)->extra, "linked_paths"), ==, 1);
}

/* Each rule: what it matches, and what only looks like it. */
static void
test_leftover_rules(Fixture *fx, gconstpointer data)
{
	static const gint64 big = 200 * 1048576;
	static const struct
	{
		const gchar *rule;
		const gchar *expected;
	} cases[] = {
		{ "sample", "a.sample.mkv|sample.mkv|zz Sample (2000).mkv|inside.mkv" },
		{ "extras", "Film-trailer.mp4|bts.mkv|poster.jpg|trailer.mp4" },
		{ "subtitle", "Alone.srt|English.srt|Film2.srt|loose.ass" },
		{ "junk", "._hidden|Thumbs.db|desktop.ini|link.url|RARBG.txt|sums.sfv" },
	};
	guint i;
	gint64 inode = 100000;
	gint64 films = fx->s.films_a;
	gint64 down = fx->s.down_a;

	SKIP_WITHOUT_DB(fx);
	axis_test_db_exec(fx->t->db, "DELETE FROM library_file");

	/* Samples: the word "sample" in a small video's name, or a video in a
	 * folder called Sample(s); not a larger film with the word in its title,
	 * not another word that contains it, not a sample under downloads. */
	insert_file(fx, films, "Film/sample.mkv", "video", 100, inode++);
	insert_file(fx, films, "Film/a.sample.mkv", "video", 100, inode++);
	insert_file(fx, films, "zz Sample (2000).mkv", "video", 100, inode++);
	insert_file(fx, films, "Film/Samples/inside.mkv", "video", big, inode++);
	insert_file(fx, films, "Film/A Sample of Honey (2000).mkv", "video", big, inode++);
	insert_file(fx, films, "Film/Example.mkv", "video", 100, inode++);
	insert_file(fx, films, "Film/Sampler.mkv", "video", 100, inode++);
	insert_file(fx, films, "Film/Samplesize.mkv", "video", 100, inode++);
	insert_file(fx, films, "Film/sample.txt", "text", 100, inode++);
	insert_file(fx, down, "Film/sample.mkv", "video", 100, inode++);
	/* Extras: in a folder of that name, or named like Plex's. */
	insert_file(fx, films, "Film/Extras/bts.mkv", "video", 100, inode++);
	insert_file(fx, films, "Film/Extras/poster.jpg", "image", 100, inode++);
	insert_file(fx, films, "Film/Trailers/trailer.mp4", "video", 100, inode++);
	insert_file(fx, films, "Film/Film-trailer.mp4", "video", 100, inode++);
	insert_file(fx, films, "Film/Extra Large (2001).mkv", "video", 100, inode++);
	insert_file(fx, films, "Film/Short-Stay 12 (2013).mkv", "video", 100, inode++);
	insert_file(fx, films, "Film/Trailer Yard Lads (2000).mkv", "video", 100, inode++);
	insert_file(fx, films, "Film/Other/misc.mkv", "video", 100, inode++);
	insert_file(fx, down, "Film/Extras/downloaded.mkv", "video", 100, inode++);
	/* Subtitles. */
	insert_file(fx, films, "Film/Film.mkv", "video", 100, inode++);
	insert_file(fx, films, "Film/Film.en.srt", "subtitle", 10, inode++);
	insert_file(fx, films, "Film/Film.srt", "subtitle", 10, inode++);
	insert_file(fx, films, "Film/FILM.FR.SRT", "subtitle", 10, inode++);
	insert_file(fx, films, "Film/Film_2.srt", "subtitle", 10, inode++);
	insert_file(fx, films, "Film/Film2.srt", "subtitle", 10, inode++);
	insert_file(fx, films, "Film/Subs/English.srt", "subtitle", 10, inode++);
	insert_file(fx, films, "Film/Subtitles/forced.srt", "subtitle", 10, inode++);
	insert_file(fx, films, "Lone/Subs/English.srt", "subtitle", 10, inode++);
	insert_file(fx, films, "Alone/Alone.srt", "subtitle", 10, inode++);
	insert_file(fx, films, "loose.ass", "subtitle", 10, inode++);
	insert_file(fx, down, "Film/Film.en.srt", "subtitle", 10, inode++);
	/* Junk. */
	insert_file(fx, films, "Film/Thumbs.db", "other", 5, inode++);
	insert_file(fx, films, "Film/desktop.ini", "other", 5, inode++);
	insert_file(fx, films, "Film/._hidden", "other", 5, inode++);
	insert_file(fx, films, "Film/link.url", "other", 5, inode++);
	insert_file(fx, films, "Film/sums.sfv", "text", 5, inode++);
	insert_file(fx, films, "Film/RARBG.txt", "text", 5, inode++);
	insert_file(fx, films, "Film/Thumbs.db.bak", "other", 5, inode++);
	insert_file(fx, films, "Film/notes.url.txt", "text", 5, inode++);
	insert_file(fx, films, "Film/RARBG documentary (2010).mkv", "video", 100, inode++);
	insert_file(fx, down, "Thumbs.db", "other", 5, inode++);

	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		g_autofree gchar *query = g_strdup_printf("leftover=%s", cases[i].rule);
		g_autofree gchar *names = list_names(fx, query);
		g_auto(GStrv) want = g_strsplit(cases[i].expected, "|", -1);
		g_auto(GStrv) have = g_strsplit(names, "|", -1);
		guint k;

		/* The same files, whatever the order. */
		g_assert_cmpuint(g_strv_length(want), ==, g_strv_length(have));
		for (k = 0; want[k] != NULL; k++) {
			if (!g_strv_contains((const gchar *const *)have, want[k]))
				g_error("rule %s: expected '%s' among %s, got %s", cases[i].rule, want[k], cases[i].expected, names);
		}
	}
}

/* ── Coverage ───────────────────────────────────────────────────── */

/* Spec: Partial data is labelled (files not probed, probes failed, roots
 * never scanned, uncertain HDR, files that changed since they were probed). */
static void
test_coverage(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisAnalysisSnapshot) snapshot = NULL;
	g_autoptr(AxisAnalysisSnapshot) again = NULL;
	JsonObject *coverage;
	JsonArray *unscanned;
	gint64 one;

	SKIP_WITHOUT_DB(fx);
	snapshot = fresh(fx);
	coverage = snapshot->coverage;

	g_assert_cmpint(axis_analysis_extra_int(coverage, "present_paths"), ==, 32);
	g_assert_cmpint(axis_analysis_extra_int(coverage, "media_paths"), ==, 29);
	/* Four videos never probed (the sample, Unknown Film twice, Example and
	 * Sampler: five), one failed. */
	g_assert_cmpint(axis_analysis_extra_int(coverage, "not_probed"), ==, 5);
	g_assert_cmpint(axis_analysis_extra_int(coverage, "probe_failed"), ==, 1);
	g_assert_cmpint(axis_analysis_extra_int(coverage, "probe_timeout"), ==, 0);
	g_assert_cmpint(axis_analysis_extra_int(coverage, "hdr_uncertain"), ==, 1);
	g_assert_cmpint(axis_analysis_extra_int(coverage, "probe_stale"), ==, 0);
	g_assert_cmpint(axis_analysis_extra_int(coverage, "suspected_paths"), ==, 0);
	g_assert_cmpint(axis_analysis_extra_int(coverage, "missing_paths"), ==, 0);
	g_assert_cmpint(axis_analysis_extra_int(coverage, "roots"), ==, 7);
	unscanned = json_object_get_array_member(coverage, "roots_unscanned");
	g_assert_cmpuint(json_array_get_length(unscanned), ==, 0);
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(coverage, "volumes_unavailable")), ==, 0);

	/* The figures link to lists of the same length. */
	g_assert_cmpint(list_count(fx, "probe=none"), ==, 5);
	g_assert_cmpint(list_count(fx, "probe=failed"), ==, 1);
	g_assert_cmpint(list_count(fx, "hdr=uncertain"), ==, 1);

	/* A root that was never scanned, and a file that changed since it was
	 * probed. */
	axis_test_library_add_root(fx->t, fx->s.vol_b, fx->s.disk_b, "Never", AXIS_MEDIA_OTHER);
	axis_test_db_exec(fx->t->db, "UPDATE library_file SET changed = true WHERE rel_path = 'Show Y/Season 1/Show Y - S01E01.mkv'");
	again = fresh(fx);
	unscanned = json_object_get_array_member(again->coverage, "roots_unscanned");
	g_assert_cmpuint(json_array_get_length(unscanned), ==, 1);
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(unscanned, 0), "name"), ==, "Never");
	one = axis_analysis_extra_int(again->coverage, "probe_stale");
	g_assert_cmpint(one, ==, 1);
	g_assert_cmpint(list_count(fx, "changed=yes&probe=ok"), ==, one);
}

/* Present only in totals; suspected and missing files on their own lines. */
static void
test_suspected_and_missing(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisAnalysisSnapshot) snapshot = NULL;
	gint64 suspected;
	gint64 missing;

	SKIP_WITHOUT_DB(fx);
	suspected = axis_test_scenario_file(fx->t, fx->s.shows_a, "Show Y/Season 1/Show Y - S01E01.mkv");
	missing = axis_test_scenario_file(fx->t, fx->s.films_a, "Hearth (1986)/Hearth (1986).mkv");
	axis_test_db_exec(fx->t->db, "UPDATE library_file SET state = 'suspected', suspected_at = now(),"
		" suspected_scan_id = (SELECT max(id) FROM library_scan) WHERE rel_path = 'Show Y/Season 1/Show Y - S01E01.mkv'");
	axis_test_db_exec(fx->t->db, "UPDATE library_file SET state = 'missing', suspected_at = now(), missing_at = now(),"
		" suspected_scan_id = (SELECT max(id) FROM library_scan) WHERE rel_path = 'Hearth (1986)/Hearth (1986).mkv'");
	g_assert_cmpint(suspected, >, 0);
	g_assert_cmpint(missing, >, 0);

	snapshot = fresh(fx);
	/* Neither is in any total or breakdown. */
	assert_fact(snapshot, "overall", "", NULL, 30, AXIS_TEST_SCENARIO_BYTES - 250 - 1100);
	assert_fact(snapshot, "comp_vcodec", "h264", NULL, 15, 8334 - 250 - 1100);
	/* They have their own lines. */
	g_assert_cmpint(axis_analysis_extra_int(snapshot->coverage, "suspected_paths"), ==, 1);
	g_assert_cmpint(axis_analysis_extra_int(snapshot->coverage, "suspected_bytes"), ==, 250);
	g_assert_cmpint(axis_analysis_extra_int(snapshot->coverage, "missing_paths"), ==, 1);
	g_assert_cmpint(axis_analysis_extra_int(snapshot->coverage, "missing_bytes"), ==, 1100);
	g_assert_cmpint(list_count(fx, "state=suspected"), ==, 1);
	g_assert_cmpint(list_count(fx, "state=missing"), ==, 1);
	g_assert_cmpint(list_count(fx, ""), ==, 30);
}

/* Spec: Offline volume does not change totals. */
static void
test_offline_volume(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisAnalysisSnapshot) before = NULL;
	g_autoptr(AxisAnalysisSnapshot) away = NULL;
	g_autoptr(AxisAnalysisSnapshot) back = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *b = NULL;
	JsonArray *unavailable;

	SKIP_WITHOUT_DB(fx);
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*is offline at*");
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*does not match what is at*");
	b = id_text(fx->s.vol_b);
	before = fresh(fx);

	axis_test_library_unplug(fx->t, fx->s.disk_b, "away-b");
	g_assert_true(axis_storage_check_volume(fx->t->storage, fx->s.vol_b, NULL, &error));
	g_assert_no_error(error);
	away = fresh(fx);

	/* Nothing moved: the same totals, the volume's figures marked. */
	g_assert_cmpint(away->paths, ==, before->paths);
	g_assert_cmpint(away->bytes, ==, before->bytes);
	assert_fact(away, "overall", "", NULL, AXIS_TEST_SCENARIO_PATHS, AXIS_TEST_SCENARIO_BYTES);
	assert_fact(away, "space_volume", b, NULL, AXIS_TEST_SCENARIO_PATHS_B, AXIS_TEST_SCENARIO_BYTES_B);
	g_assert_cmpint(axis_analysis_extra_int(fact(away, "space_volume", b, NULL)->extra, "usable"), ==, 0);
	g_assert_cmpstr(axis_analysis_extra_string(fact(away, "space_volume", b, NULL)->extra, "state"), ==, "offline");
	assert_fact(away, "comp_vcodec", "h264", NULL, 17, 8334);
	g_assert_cmpint(axis_analysis_extra_int(fact(away, "overall", "", NULL)->extra, "offline_paths"), ==, 8);
	g_assert_cmpint(axis_analysis_extra_int(fact(away, "overall", "", NULL)->extra, "offline_bytes"), ==,
		AXIS_TEST_SCENARIO_BYTES_B);
	/* And the coverage says so, with what the volume holds. */
	unavailable = json_object_get_array_member(away->coverage, "volumes_unavailable");
	g_assert_cmpuint(json_array_get_length(unavailable), ==, 1);
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(unavailable, 0), "name"), ==, "Disk B");
	g_assert_cmpint(json_object_get_int_member(json_array_get_object_element(unavailable, 0), "paths"), ==, 8);
	g_assert_cmpint(json_object_get_int_member(json_array_get_object_element(unavailable, 0), "bytes"), ==, 5637);
	/* The duplicates across the away volume are still found, marked. */
	{
		JsonArray *groups = items(fact(away, "dup_content", "", NULL));
		JsonArray *members = json_object_get_array_member(json_array_get_object_element(groups, 0), "members");
		guint i;
		guint marked = 0;

		for (i = 0; i < json_array_get_length(members); i++)
			if (!json_object_get_boolean_member(json_array_get_object_element(members, i), "usable"))
				marked++;
		g_assert_cmpuint(marked, ==, 1);
	}

	axis_test_library_plug(fx->t, fx->s.disk_b, "away-b");
	g_assert_true(axis_storage_check_volume(fx->t->storage, fx->s.vol_b, NULL, &error));
	back = fresh(fx);
	g_assert_cmpint(back->bytes, ==, before->bytes);
	g_assert_cmpint(axis_analysis_extra_int(fact(back, "space_volume", b, NULL)->extra, "usable"), ==, 1);
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(back->coverage, "volumes_unavailable")), ==, 0);
}

/* ── Snapshots ──────────────────────────────────────────────────── */

typedef struct
{
	Fixture *fx;
	gint     scanned;
	gint64   during_paths;
} ScanDuring;

static void
scan_during_refresh(
	const gchar *stage,
	gpointer     data
){
	ScanDuring *during = data;
	g_autofree gchar *path = NULL;
	AxisScanResult *result;

	if (during->scanned != 0 || g_strcmp0(stage, "space by root") != 0)
		return;
	during->scanned = 1;
	/* A new file appears while the refresh holds its view of the index,
	 * and a scan of its root runs to its end. */
	axis_test_scenario_put(during->fx->s.disk_a, "Shows", "Show Y/Season 1/Show Y - S01E02.mkv", 99);
	result = axis_test_library_scan_expect(during->fx->t, during->fx->s.shows_a, AXIS_SCAN_COMPLETE, AXIS_SCAN_REASON_NONE);
	axis_scan_result_free(result);
	during->scanned = 2;
	during->during_paths = axis_test_library_n(during->fx->t, "SELECT count(*) FROM library_file WHERE state = 'present'");
}

/* Spec: Refresh does not block scanning; the snapshot is of one moment. */
static void
test_refresh_does_not_block_scans(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisAnalysisSnapshot) snapshot = NULL;
	g_autoptr(AxisAnalysisSnapshot) next = NULL;
	AxisAnalysisTuning tuning;
	ScanDuring during;

	SKIP_WITHOUT_DB(fx);
	memset(&during, 0, sizeof(during));
	during.fx = fx;
	memset(&tuning, 0, sizeof(tuning));
	tuning.after_stage = scan_during_refresh;
	tuning.data = &during;
	axis_analysis_set_tuning(fx->analysis, &tuning);

	snapshot = fresh(fx);
	/* The scan ran to its end in the middle of the refresh (a blocked scan
	 * would have timed out and failed the test)... */
	g_assert_cmpint(during.scanned, ==, 2);
	g_assert_cmpint(during.during_paths, ==, AXIS_TEST_SCENARIO_PATHS + 1);
	/* ...and the refresh did not see it: all of its figures are of the index
	 * as it was when it began. */
	g_assert_cmpint(snapshot->paths, ==, AXIS_TEST_SCENARIO_PATHS);
	assert_fact(snapshot, "space_kind", "video", NULL, 25, 16295);
	{
		g_autofree gchar *shows = id_text(fx->s.shows_a);

		assert_fact(snapshot, "space_root", shows, NULL, 3, 860);
	}

	memset(&tuning, 0, sizeof(tuning));
	axis_analysis_set_tuning(fx->analysis, &tuning);
	next = fresh(fx);
	g_assert_cmpint(next->paths, ==, AXIS_TEST_SCENARIO_PATHS + 1);
	g_assert_cmpint(next->bytes, ==, AXIS_TEST_SCENARIO_BYTES + 99);
}

/* Two refreshes do not run at once. */
static void
test_one_refresh_at_a_time(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	AxisDbConn *conn;

	SKIP_WITHOUT_DB(fx);
	/* Another session holds the refresh's lock. */
	conn = axis_db_pool_acquire(fx->t->pool, &error);
	g_assert_nonnull(conn);
	g_assert_true(axis_db_conn_begin(conn, &error));
	g_assert_true(axis_db_conn_exec(conn, "SELECT pg_advisory_xact_lock(hashtext('axis.analysis.refresh'), 0)", 0,
		NULL, NULL, &error));

	g_assert_false(axis_analysis_refresh(fx->analysis, "direct", 0, NULL, NULL, &error));
	g_assert_error(error, AXIS_ANALYSIS_ERROR, AXIS_ANALYSIS_ERROR_BUSY);
	g_clear_error(&error);
	/* Nothing was stored. */
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM analysis_snapshot"), ==, 0);
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(fx->t->pool, conn);

	g_assert_true(axis_analysis_refresh(fx->analysis, "direct", 0, NULL, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM analysis_snapshot"), ==, 1);
	/* Nothing is left in the work area. */
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM pg_class WHERE relname = 'analysis_work'"), ==, 0);
}

/* A snapshot stores aggregates and bounded lists only. */
static void
test_snapshot_is_small(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisAnalysisSnapshot) snapshot = NULL;
	gint64 facts;

	SKIP_WITHOUT_DB(fx);
	snapshot = fresh(fx);
	facts = axis_test_library_n(fx->t, "SELECT count(*) FROM analysis_fact WHERE snapshot_id = %" G_GINT64_FORMAT,
		snapshot->id);
	g_assert_cmpint(facts, >, 20);
	/* Far fewer rows than there are files. */
	g_assert_cmpint(facts, <, 80);
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT max(jsonb_array_length(extra->'items')) FROM analysis_fact"
		" WHERE extra ? 'items'"), <=, 100);
}

/* The last snapshot of each day is kept long, the others a few days. */
static void
test_retention(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisAnalysisSnapshot) snapshot = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *newest = NULL;
	gint64 early;
	gint64 late;
	gint64 old;
	gint64 recent;
	gint64 recent_late;

	SKIP_WITHOUT_DB(fx);
	/* UTC days, whatever the time zone of the session. */
#define INSERT_SNAPSHOT(when) axis_test_library_n(fx->t, \
		"INSERT INTO analysis_snapshot (taken_at, trigger, took_ms, paths, bytes, coverage, config)" \
		" VALUES ((" when ") AT TIME ZONE 'UTC', 'schedule', 0, 0, 0, '{}', '{}') RETURNING id")
#define UTC_DAY(days) "date_trunc('day', (now() AT TIME ZONE 'UTC') - interval '" days " days')"
	early = INSERT_SNAPSHOT(UTC_DAY("10") " + interval '3 hours'");
	late = INSERT_SNAPSHOT(UTC_DAY("10") " + interval '20 hours'");
	old = INSERT_SNAPSHOT("(now() AT TIME ZONE 'UTC') - interval '500 days'");
	recent = INSERT_SNAPSHOT(UTC_DAY("2") " + interval '1 hour'");
	recent_late = INSERT_SNAPSHOT(UTC_DAY("2") " + interval '5 hours'");
#undef INSERT_SNAPSHOT
#undef UTC_DAY
	axis_test_db_exec(fx->t->db, "INSERT INTO analysis_fact (snapshot_id, report) SELECT id, 'overall' FROM analysis_snapshot");

	snapshot = fresh(fx);
	/* Ten days on: the last of a day stays, the one before it goes; a
	 * year and a half on goes; recent ones stay; the new one stays. */
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM analysis_snapshot WHERE id = %" G_GINT64_FORMAT, early), ==, 0);
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM analysis_snapshot WHERE id = %" G_GINT64_FORMAT, late), ==, 1);
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM analysis_snapshot WHERE id = %" G_GINT64_FORMAT, old), ==, 0);
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM analysis_snapshot WHERE id = %" G_GINT64_FORMAT, recent), ==, 1);
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM analysis_snapshot WHERE id = %" G_GINT64_FORMAT, recent_late), ==, 1);
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM analysis_snapshot WHERE id = %" G_GINT64_FORMAT, snapshot->id), ==, 1);
	/* The facts go with their snapshot. */
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM analysis_fact WHERE snapshot_id = %" G_GINT64_FORMAT, early), ==, 0);
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM analysis_fact WHERE snapshot_id = %" G_GINT64_FORMAT, late), ==, 1);

	/* Shorter retention is a setting: now the earlier of the two on a recent
	 * day goes too, and the last of that day stays. */
	g_assert_true(axis_settings_set(fx->settings, AXIS_ANALYSIS_SETTING_KEEP_OTHER, "1d", &error));
	g_assert_no_error(error);
	refresh(fx);
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM analysis_snapshot WHERE id = %" G_GINT64_FORMAT, recent), ==, 0);
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM analysis_snapshot WHERE id = %" G_GINT64_FORMAT, recent_late), ==, 1);
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM analysis_snapshot WHERE id = %" G_GINT64_FORMAT, late), ==, 1);
	/* The first of today's two is not the last of its day either, but it is
	 * younger than a day: it stays. */
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM analysis_snapshot WHERE id = %" G_GINT64_FORMAT, snapshot->id), ==, 1);
	newest = axis_test_library_q(fx->t, "SELECT max(id) FROM analysis_snapshot");
	g_assert_cmpint(g_ascii_strtoll(newest, NULL, 10), >, snapshot->id);
}

/* Spec: Growth. */
static void
test_history(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) points = NULL;
	const AxisAnalysisPoint *first;

	SKIP_WITHOUT_DB(fx);
	axis_test_db_exec(fx->t->db,
		"INSERT INTO analysis_snapshot (taken_at, trigger, took_ms, paths, bytes, volumes_total, volumes_used, coverage, config)"
		" SELECT now() - make_interval(days => d), 'schedule', 0, 10 * d, 1000 * d, 100000, 1000 * d, '{}', '{}'"
		" FROM generate_series(3, 5) d");
	refresh(fx);

	g_assert_true(axis_analysis_load_history(fx->analysis, 400, &points, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(points->len, ==, 4);
	first = g_ptr_array_index(points, 0);
	g_assert_cmpint(first->bytes, ==, 5000);
	g_assert_cmpint(first->volumes_used, ==, 5000);
	/* Oldest first, the new snapshot last. */
	{
		const AxisAnalysisPoint *last = g_ptr_array_index(points, 3);
		guint i;

		g_assert_cmpint(last->bytes, ==, AXIS_TEST_SCENARIO_BYTES);
		for (i = 1; i < points->len; i++)
			g_assert_cmpint(((const AxisAnalysisPoint *)g_ptr_array_index(points, i))->taken_us, >=,
				((const AxisAnalysisPoint *)g_ptr_array_index(points, i - 1))->taken_us);
	}
	g_clear_pointer(&points, g_ptr_array_unref);
	g_assert_true(axis_analysis_load_history(fx->analysis, 2, &points, &error));
	g_assert_cmpuint(points->len, ==, 2);
}

/* ── When a refresh is due ──────────────────────────────────────── */

static gboolean
due(Fixture *fx)
{
	g_autoptr(GError) error = NULL;
	gboolean enqueued = FALSE;

	g_assert_true(axis_analysis_enqueue_due(fx->analysis, &enqueued, &error));
	g_assert_no_error(error);
	return enqueued;
}

static gint64
queued_refreshes(Fixture *fx)
{
	return axis_test_library_n(fx->t, "SELECT count(*) FROM job WHERE kind = 'analysis.refresh'"
		" AND state IN ('queued', 'running')");
}

/* After scans and probes settle, debounced, at most once an interval. */
static void
test_due(Fixture *fx, gconstpointer data)
{
	SKIP_WITHOUT_DB(fx);

	/* The scans ended a moment ago: not settled. */
	g_assert_false(due(fx));
	g_assert_cmpint(queued_refreshes(fx), ==, 0);

	/* An hour on, with nothing running, and no report yet: due. */
	axis_test_db_exec(fx->t->db, "UPDATE library_scan SET started_at = started_at - interval '1 hour',"
		" finished_at = finished_at - interval '1 hour'");
	g_assert_true(due(fx));
	g_assert_cmpint(queued_refreshes(fx), ==, 1);
	{
		g_autofree gchar *trigger = axis_test_library_q(fx->t,
			"SELECT payload->>'trigger' FROM job WHERE kind = 'analysis.refresh'");

		g_assert_cmpstr(trigger, ==, "schedule");
	}
	/* One job: asking again adds none. */
	g_assert_false(due(fx));
	g_assert_cmpint(queued_refreshes(fx), ==, 1);
	axis_test_db_exec(fx->t->db, "DELETE FROM job");

	/* A probe still queued: scans and probes have not settled. */
	axis_test_db_exec(fx->t->db, "INSERT INTO job (kind, queue, max_attempts) VALUES ('probe.file', 'probe', 1)");
	g_assert_false(due(fx));
	axis_test_db_exec(fx->t->db, "UPDATE job SET state = 'succeeded', finished_at = now() - interval '30 minutes'");
	g_assert_true(due(fx));
	axis_test_db_exec(fx->t->db, "DELETE FROM job WHERE kind = 'analysis.refresh'");
	/* The probe ended a moment ago: the debounce waits for it to settle. */
	axis_test_db_exec(fx->t->db, "UPDATE job SET finished_at = now() - interval '1 minute'");
	g_assert_false(due(fx));
	axis_test_db_exec(fx->t->db, "DELETE FROM job");

	/* A scan running. */
	axis_test_db_exec(fx->t->db, "UPDATE library_scan SET status = 'running', reason = NULL, finished_at = NULL"
		" WHERE id = (SELECT min(id) FROM library_scan)");
	g_assert_false(due(fx));
	axis_test_db_exec(fx->t->db, "UPDATE library_scan SET status = 'complete', finished_at = now() - interval '1 hour'"
		" WHERE id = (SELECT min(id) FROM library_scan)");

	/* With a report, nothing changed since: not due.  A scan that ended
	 * after the report: not due too soon after it, due an interval on. */
	refresh(fx);
	g_assert_false(due(fx));
	axis_test_db_exec(fx->t->db, "UPDATE analysis_snapshot SET taken_at = now() - interval '30 minutes'");
	axis_test_db_exec(fx->t->db, "UPDATE library_scan SET finished_at = now() - interval '20 minutes'"
		" WHERE id = (SELECT max(id) FROM library_scan)");
	g_assert_false(due(fx));
	axis_test_db_exec(fx->t->db, "UPDATE analysis_snapshot SET taken_at = now() - interval '2 hours'");
	g_assert_true(due(fx));
	axis_test_db_exec(fx->t->db, "DELETE FROM job");

	/* Nothing changed, but the report is a day old: one a day is made. */
	axis_test_db_exec(fx->t->db, "UPDATE library_scan SET finished_at = now() - interval '3 days'");
	axis_test_db_exec(fx->t->db, "UPDATE analysis_snapshot SET taken_at = now() - interval '23 hours'");
	g_assert_false(due(fx));
	axis_test_db_exec(fx->t->db, "UPDATE analysis_snapshot SET taken_at = now() - interval '25 hours'");
	g_assert_true(due(fx));
}

/* Nothing is due without a root. */
static void
test_due_without_roots(Fixture *fx, gconstpointer data)
{
	AxisTestLibrary *t;
	g_autoptr(AxisAnalysis) analysis = NULL;
	g_autoptr(GError) error = NULL;
	gboolean enqueued = TRUE;

	SKIP_WITHOUT_DB(fx);
	t = axis_test_library_new(4);
	analysis = axis_analysis_new(t->pool, NULL);
	g_assert_true(axis_analysis_enqueue_due(analysis, &enqueued, &error));
	g_assert_no_error(error);
	g_assert_false(enqueued);
	/* Without settings the defaults are used. */
	g_assert_true(axis_analysis_refresh(analysis, "direct", 0, NULL, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpint(axis_test_library_n(t, "SELECT paths FROM analysis_snapshot"), ==, 0);
	axis_analysis_unref(g_steal_pointer(&analysis));
	axis_test_library_free(t);
}

static gboolean
refresh_handler(
	AxisJobContext  *context,
	gpointer         user_data,
	GError         **error
){
	return axis_analysis_run_refresh_job(user_data, context, error);
}

/* The refresh as a job of the queue: requested, run, recorded. */
static void
test_refresh_job(Fixture *fx, gconstpointer data)
{
	static const AxisJobHandlerInfo handler = {
		AXIS_ANALYSIS_REFRESH_KIND, AXIS_JOB_MAINTENANCE_QUEUE, AXIS_ANALYSIS_REFRESH_ATTEMPTS, refresh_handler,
		"Updating the analysis", AXIS_JOB_CLASS_WORK
	};
	g_autoptr(AxisJobRegistry) registry = NULL;
	g_autoptr(AxisJobQueue) queue = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *trigger = NULL;
	g_autofree gchar *job_text = NULL;
	gint64 job = 0;
	gint64 again = 0;

	SKIP_WITHOUT_DB(fx);
	registry = axis_job_registry_new();
	g_assert_true(axis_job_registry_add(registry, &handler, fx->analysis, &error));
	queue = axis_job_queue_new(fx->t->pool, registry);
	axis_job_queue_set_poll_interval(queue, 50);

	/* Asked twice, one job. */
	g_assert_true(axis_analysis_request_refresh(fx->analysis, &job, &error));
	g_assert_true(axis_analysis_request_refresh(fx->analysis, &again, &error));
	g_assert_no_error(error);
	g_assert_cmpint(job, >, 0);
	g_assert_cmpint(job, ==, again);
	{
		AxisAnalysisStatus status;

		g_assert_true(axis_analysis_get_status(fx->analysis, &status, &error));
		g_assert_true(status.refreshing);
		g_assert_false(status.running);
		g_assert_cmpint(status.job_id, ==, job);
		axis_analysis_status_clear(&status);
	}

	g_assert_true(axis_job_queue_start(queue, &error));
	g_assert_no_error(error);
	g_assert_true(axis_test_wait_query(fx->t->db, "SELECT state::text FROM job WHERE kind = 'analysis.refresh'",
		"succeeded", AXIS_TEST_WAIT_MS));
	g_assert_true(axis_job_queue_stop(queue, 5000));

	trigger = axis_test_library_q(fx->t, "SELECT trigger FROM analysis_snapshot");
	job_text = axis_test_library_q(fx->t, "SELECT job_id FROM analysis_snapshot");
	g_assert_cmpstr(trigger, ==, "request");
	g_assert_cmpint(g_ascii_strtoll(job_text, NULL, 10), ==, job);
	{
		AxisAnalysisStatus status;

		g_assert_true(axis_analysis_get_status(fx->analysis, &status, &error));
		g_assert_false(status.refreshing);
		axis_analysis_status_clear(&status);
	}
}

/* Settings: the module's declarations are valid and have the defaults. */
static void
test_settings(Fixture *fx, gconstpointer data)
{
	SKIP_WITHOUT_DB(fx);
	g_assert_cmpfloat_with_epsilon(axis_settings_get_decimal(fx->settings, AXIS_ANALYSIS_SETTING_LARGE_H264), 0.30, 0.0001);
	g_assert_cmpint(axis_settings_get_duration(fx->settings, AXIS_ANALYSIS_SETTING_MIN_INTERVAL), ==, 3600);
	g_assert_cmpint(axis_settings_get_duration(fx->settings, AXIS_ANALYSIS_SETTING_SETTLE), ==, 600);
	g_assert_cmpint(axis_settings_get_duration(fx->settings, AXIS_ANALYSIS_SETTING_KEEP_DAILY), ==, 400 * 24 * 3600);
	g_assert_cmpint(axis_settings_get_duration(fx->settings, AXIS_ANALYSIS_SETTING_KEEP_OTHER), ==, 7 * 24 * 3600);
	/* Out of range is refused. */
	g_assert_false(axis_settings_check(fx->settings, AXIS_ANALYSIS_SETTING_LARGE_H264, "0", NULL));
	g_assert_false(axis_settings_check(fx->settings, AXIS_ANALYSIS_SETTING_LARGE_H264, "9", NULL));
	g_assert_false(axis_settings_check(fx->settings, AXIS_ANALYSIS_SETTING_SETTLE, "1s", NULL));
	g_assert_true(axis_settings_check(fx->settings, AXIS_ANALYSIS_SETTING_SETTLE, "30m", NULL));
}

#define ADD(name, func) g_test_add("/analysis/" name, Fixture, NULL, fixture_setup, func, fixture_teardown)

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	ADD("space", test_space);
	ADD("figures-match-lists", test_figures_match_lists);
	ADD("hardlinks-are-not-duplicates", test_hardlinks_are_not_duplicates);
	ADD("composition", test_composition);
	ADD("outliers", test_outliers);
	ADD("largest", test_largest);
	ADD("duplicates-content", test_duplicates_content);
	ADD("duplicates-title", test_duplicates_title);
	ADD("duplicate-links", test_duplicate_links);
	ADD("split", test_split);
	ADD("leftovers", test_leftovers);
	ADD("hardlink-attribution", test_hardlink_attribution);
	ADD("leftover-rules", test_leftover_rules);
	ADD("coverage", test_coverage);
	ADD("suspected-and-missing", test_suspected_and_missing);
	ADD("offline-volume", test_offline_volume);
	ADD("refresh-does-not-block-scans", test_refresh_does_not_block_scans);
	ADD("one-refresh-at-a-time", test_one_refresh_at_a_time);
	ADD("snapshot-is-small", test_snapshot_is_small);
	ADD("retention", test_retention);
	ADD("history", test_history);
	ADD("due", test_due);
	ADD("due-without-roots", test_due_without_roots);
	ADD("refresh-job", test_refresh_job);
	ADD("settings", test_settings);

	return g_test_run();
}
