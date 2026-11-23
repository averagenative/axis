/*
 * test-estimate-db.c - the estimator over a real index: the decisions for
 * every file of tests/axis-test-savings.h, the one loader against the
 * probe's own, snapshots whose totals are the sums of the files' estimates,
 * hard links counted once, a volume going away, file lists that agree with
 * the figures, calibration from recorded samples, and when a refresh is due
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <glib.h>
#include <string.h>

#include "axis-test-analysis.h"
#include "axis-test-db.h"
#include "axis-test-library.h"
#include "axis-test-savings.h"
#include "library/axis-analysis.h"
#include "library/axis-library-filter.h"
#include "probe/axis-probe.h"
#include "transcode/axis-estimator.h"
#include "transcode/axis-estimator-load.h"

typedef struct
{
	AxisTestLibrary *t;
	AxisSettings    *settings;
	AxisEstimator   *estimator;
	AxisProbe       *probe;
	AxisTestSavings  s;
} Fixture;

#define SKIP_WITHOUT_DB(fx) do { if ((fx)->t == NULL) return; } while (0)

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;

	memset(fx, 0, sizeof(*fx));
	axis_test_log_init();
	axis_test_log_reset();
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*is offline at*");
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*does not match what is at*");
	fx->t = axis_test_library_new(8);
	if (fx->t == NULL)
		return;
	fx->settings = axis_settings_new();
	g_assert_true(axis_settings_add_table(fx->settings, AXIS_ESTIMATE_MODULE_ID, axis_estimator_get_setting_table(),
		&error));
	g_assert_true(axis_settings_add_table(fx->settings, AXIS_ANALYSIS_MODULE_ID, axis_analysis_get_setting_table(),
		&error));
	g_assert_true(axis_settings_load(fx->settings, NULL, &error));
	g_assert_no_error(error);
	fx->estimator = axis_estimator_new(fx->t->pool, fx->t->storage, fx->settings);
	fx->probe = axis_probe_new(fx->t->pool, fx->t->storage, NULL);
	axis_test_savings_build(fx->t, &fx->s);
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	if (fx->t == NULL) {
		axis_test_log_reset();
		return;
	}
	axis_probe_unref(fx->probe);
	axis_estimator_unref(fx->estimator);
	axis_settings_unref(fx->settings);
	axis_test_savings_clear(&fx->s);
	axis_test_library_free(fx->t);
	axis_test_log_reset();
}

static gint64
file_id(
	Fixture     *fx,
	gint64       root,
	const gchar *rel
){
	return axis_test_scenario_file(fx->t, root, rel);
}

/* The decision of one target for a file, as the file page has it; a
 * target of the other kind (the page does not list it) is not applicable. */
static AxisEstimateResult
decide(
	Fixture     *fx,
	gint64       id,
	const gchar *target
){
	g_autoptr(GError) error = NULL;
	g_autoptr(GArray) estimates = NULL;
	AxisEstimateResult none;
	guint i;

	g_assert_true(axis_estimator_estimate_file(fx->estimator, id, &estimates, &error));
	g_assert_no_error(error);
	for (i = 0; i < estimates->len; i++) {
		const AxisFileEstimate *e = &g_array_index(estimates, AxisFileEstimate, i);

		if (strcmp(e->target.id, target) == 0)
			return e->result;
	}
	memset(&none, 0, sizeof(none));
	none.reason = AXIS_ESTIMATE_NOT_APPLICABLE;
	return none;
}

static void
expect(
	Fixture            *fx,
	gint64              root,
	const gchar        *rel,
	const gchar        *target,
	AxisEstimateReason  reason
){
	AxisEstimateResult r = decide(fx, file_id(fx, root, rel), target);

	if (r.reason != reason)
		g_error("%s for %s: expected %s, got %s", rel, target, axis_estimate_reason_to_string(reason),
			axis_estimate_reason_to_string(r.reason));
}

static gint64
refresh(Fixture *fx)
{
	g_autoptr(GError) error = NULL;
	gint64 id = 0;

	g_assert_true(axis_estimator_refresh(fx->estimator, "direct", 0, NULL, &id, &error));
	g_assert_no_error(error);
	g_assert_cmpint(id, >, 0);
	return id;
}

static AxisEstimateSnapshot *
snapshot(Fixture *fx)
{
	g_autoptr(GError) error = NULL;
	AxisEstimateSnapshot *snap = NULL;

	g_assert_true(axis_estimator_load_snapshot(fx->estimator, NULL, &snap, &error));
	g_assert_no_error(error);
	g_assert_nonnull(snap);
	return snap;
}

/* ── Decisions ──────────────────────────────────────────────────── */

static void
test_decisions(Fixture *fx, gconstpointer data)
{
	gint64 films;
	gint64 music;
	AxisEstimateResult r;

	SKIP_WITHOUT_DB(fx);
	films = fx->s.films_a;
	music = fx->s.music_a;

	expect(fx, films, "Big (2001)/Big (2001).mkv", "space_saver", AXIS_ESTIMATE_ELIGIBLE);
	expect(fx, films, "Big (2001)/Big (2001).mkv", "balanced", AXIS_ESTIMATE_ELIGIBLE);
	/* Maximum compatibility's own threshold (0.120 bits per pixel, no longer
	 * the Analysis outlier one of 0.30): Big's 0.161 is converted. */
	expect(fx, films, "Big (2001)/Big (2001).mkv", "max_compat", AXIS_ESTIMATE_ELIGIBLE);
	expect(fx, films, "Remux (2002)/Remux (2002).mkv", "space_saver", AXIS_ESTIMATE_ELIGIBLE);
	expect(fx, films, "Remux (2002)/Remux (2002).mkv", "max_compat", AXIS_ESTIMATE_LAYOUT);
	expect(fx, films, "Small (2003)/Small (2003).mkv", "space_saver", AXIS_ESTIMATE_WOULD_NOT_SHRINK);
	expect(fx, films, "Small (2003)/Small (2003).mkv", "max_compat", AXIS_ESTIMATE_ALREADY_EFFICIENT);
	expect(fx, films, "HEVC (2004)/HEVC (2004).mkv", "space_saver", AXIS_ESTIMATE_ALREADY_EFFICIENT);
	expect(fx, films, "HEVC (2004)/HEVC (2004).mkv", "max_compat", AXIS_ESTIMATE_BETTER_CODEC);
	expect(fx, films, "DV (2005)/DV (2005).mkv", "space_saver", AXIS_ESTIMATE_DOLBY_VISION);
	expect(fx, films, "DV (2005)/DV (2005).mkv", "max_compat", AXIS_ESTIMATE_DOLBY_VISION);
	expect(fx, films, "Uncertain (2006)/Uncertain (2006).mkv", "space_saver", AXIS_ESTIMATE_HDR_UNCERTAIN);
	expect(fx, films, "Unprobed.mkv", "space_saver", AXIS_ESTIMATE_NOT_PROBED);
	expect(fx, films, "Broken.mkv", "space_saver", AXIS_ESTIMATE_PROBE_FAILED);
	expect(fx, films, "Linked (2007)/Linked (2007).mkv", "space_saver", AXIS_ESTIMATE_PINNED);
	expect(fx, films, "Linked (2007)/Linked copy.mkv", "space_saver", AXIS_ESTIMATE_PINNED);
	expect(fx, fx->s.down_a, "Linked.mkv", "space_saver", AXIS_ESTIMATE_NOT_APPLICABLE);
	expect(fx, films, "HDR10 (2008)/HDR10 (2008).mkv", "space_saver", AXIS_ESTIMATE_ELIGIBLE);
	expect(fx, films, "HDR10 (2008)/HDR10 (2008).mkv", "max_compat", AXIS_ESTIMATE_HDR_TO_SDR);
	expect(fx, fx->s.shows_a, "Show (2010)/Season 1/Show - S01E01.mkv", "space_saver", AXIS_ESTIMATE_ELIGIBLE);
	expect(fx, fx->s.shows_a, "Show (2010)/Season 1/Show - S01E02.mkv", "space_saver", AXIS_ESTIMATE_INTERLACED);
	expect(fx, music, "Band/Album/01 One.flac", "music_space_saver", AXIS_ESTIMATE_ELIGIBLE);
	expect(fx, music, "Band/Album/02 Two.flac", "music_compat", AXIS_ESTIMATE_ELIGIBLE);
	expect(fx, music, "Band/Album/01 One.flac", "keep_lossless", AXIS_ESTIMATE_WOULD_NOT_SHRINK);
	expect(fx, music, "Band/Album/01 One.flac", "audiobooks", AXIS_ESTIMATE_NOT_APPLICABLE);
	expect(fx, music, "Band/Mixed/02 B.mp3", "music_space_saver", AXIS_ESTIMATE_LOSSY_SOURCE);
	r = decide(fx, file_id(fx, music, "Band/Mixed/01 A.flac"), "music_space_saver");
	g_assert_cmpint(r.reason, ==, AXIS_ESTIMATE_ALBUM);
	g_assert_cmpint(r.album_reason, ==, AXIS_ESTIMATE_LOSSY_SOURCE);
	g_assert_cmpint(r.album_file, ==, file_id(fx, music, "Band/Mixed/02 B.mp3"));
	expect(fx, music, "Loose.wav", "keep_lossless", AXIS_ESTIMATE_ELIGIBLE);
	expect(fx, music, "Loose.wav", "music_space_saver", AXIS_ESTIMATE_ELIGIBLE);
	expect(fx, fx->s.books_a, "Author/Book/Part 1.mp3", "audiobooks", AXIS_ESTIMATE_ELIGIBLE);
	expect(fx, fx->s.books_a, "Author/Book/Part 2.mp3", "music_space_saver", AXIS_ESTIMATE_NOT_APPLICABLE);
	expect(fx, fx->s.films_b, "Other (2011)/Other (2011).mkv", "space_saver", AXIS_ESTIMATE_ELIGIBLE);

	/* A file the page shows targets of its kind for. */
	{
		g_autoptr(GError) error = NULL;
		g_autoptr(GArray) estimates = NULL;

		g_assert_true(axis_estimator_estimate_file(fx->estimator, file_id(fx, films, "Big (2001)/Big (2001).mkv"),
			&estimates, &error));
		g_assert_cmpuint(estimates->len, ==, 3);
		g_clear_pointer(&estimates, g_array_unref);
		g_assert_true(axis_estimator_estimate_file(fx->estimator, file_id(fx, music, "Loose.wav"), &estimates,
			&error));
		g_assert_cmpuint(estimates->len, ==, 4);
		g_clear_pointer(&estimates, g_array_unref);
		g_assert_false(axis_estimator_estimate_file(fx->estimator, 999999, &estimates, &error));
		g_assert_error(error, AXIS_ESTIMATE_ERROR, AXIS_ESTIMATE_ERROR_NOT_FOUND);
	}
}

/* ── The loader ─────────────────────────────────────────────────── */

/* The estimator's loader and the probe's own (the file page's) read the
 * same facts: the same decision and estimate from either; and the SQL
 * album unit is the C one. */
static void
test_loader_agrees(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *ids_text = NULL;
	g_auto(GStrv) ids = NULL;
	g_autoptr(AxisEstimateModel) model = axis_estimate_model_new();
	AxisEstimateTarget targets[AXIS_ESTIMATE_N_TARGETS];
	AxisDbConn *conn;
	guint compared = 0;
	guint i;

	SKIP_WITHOUT_DB(fx);
	axis_estimator_get_targets(fx->estimator, targets);
	ids_text = axis_test_library_q(fx->t, "SELECT string_agg(id::text, ',' ORDER BY id) FROM library_file"
		" WHERE kind IN ('video', 'audio')");
	ids = g_strsplit(ids_text, ",", -1);
	conn = axis_db_pool_acquire(fx->t->pool, NULL);
	g_assert_nonnull(conn);
	for (i = 0; ids[i] != NULL; i++) {
		g_autoptr(GError) error = NULL;
		g_autoptr(AxisEstimateRows) rows = NULL;
		g_autoptr(AxisFileView) view = NULL;
		g_autofree gchar *key = NULL;
		gint64 id = g_ascii_strtoll(ids[i], NULL, 10);
		const AxisEstimateFile *loaded;
		AxisEstimateFile other;
		AxisMediaType type = AXIS_MEDIA_OTHER;
		g_autofree gchar *type_text = NULL;
		guint t;

		g_assert_true(axis_estimator_load_files(conn, &id, 1, &rows, &error));
		g_assert_no_error(error);
		g_assert_cmpuint(rows->files->len, ==, 1);
		loaded = g_ptr_array_index(rows->files, 0);
		g_assert_true(axis_probe_get_file_view(fx->probe, id, &view, &error));

		type_text = axis_test_library_q(fx->t, "SELECT r.media_type FROM library_file f JOIN library_root r"
			" ON r.id = f.root_id WHERE f.id = %" G_GINT64_FORMAT, id);
		g_assert_true(axis_media_type_from_string(type_text, &type));
		key = axis_estimate_unit_key(view->kind, type, view->rel_path);
		g_assert_cmpstr(key, ==, loaded->index.unit);

		/* The same file, from the probe's view. */
		other = *loaded;
		other.index.size = view->size;
		other.index.pinned = view->pinned;
		other.index.changed = view->changed;
		other.index.nlink = view->nlink;
		other.info = view->info;
		for (t = 0; t < AXIS_ESTIMATE_N_TARGETS; t++) {
			AxisEstimateResult a;
			AxisEstimateResult b;

			/* An album of several files is decided whole elsewhere. */
			if (loaded->index.unit_files > 1)
				continue;
			axis_estimate_eligibility(loaded, &targets[t], model, &a);
			axis_estimate_eligibility(&other, &targets[t], model, &b);
			g_assert_cmpint(a.reason, ==, b.reason);
			g_assert_cmpint(a.low, ==, b.low);
			g_assert_cmpint(a.mid, ==, b.mid);
			g_assert_cmpint(a.high, ==, b.high);
			compared++;
		}
	}
	axis_db_pool_release(fx->t->pool, conn);
	g_assert_cmpuint(compared, >, 50);
}

/* ── Snapshots ──────────────────────────────────────────────────── */

/* Every present video and audio file's decision for @target, as the file
 * page has it. */
static GArray *
all_results(
	Fixture     *fx,
	const gchar *target,
	GArray     **files
){
	g_autofree gchar *ids_text = axis_test_library_q(fx->t, "SELECT string_agg(id::text, ',' ORDER BY id)"
		" FROM library_file WHERE kind IN ('video', 'audio') AND state = 'present'");
	g_auto(GStrv) ids = g_strsplit(ids_text, ",", -1);
	GArray *out = g_array_new(FALSE, TRUE, sizeof(AxisEstimateResult));
	guint i;

	*files = g_array_new(FALSE, TRUE, sizeof(gint64));
	for (i = 0; ids[i] != NULL; i++) {
		gint64 id = g_ascii_strtoll(ids[i], NULL, 10);
		AxisEstimateResult r = decide(fx, id, target);

		g_array_append_val(out, r);
		g_array_append_val(*files, id);
	}
	return out;
}

static void
test_totals_are_sums(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisEstimateSnapshot) snap = NULL;
	const gchar *const *targets = axis_estimate_target_ids();
	guint t;

	SKIP_WITHOUT_DB(fx);
	refresh(fx);
	snap = snapshot(fx);

	for (t = 0; targets[t] != NULL; t++) {
		g_autoptr(GArray) files = NULL;
		g_autoptr(GArray) results = all_results(fx, targets[t], &files);
		const AxisEstimateFact *total = axis_estimate_snapshot_fact(snap, targets[t], "total", "");
		gint64 paths = 0;
		gint64 bytes = 0;
		gint64 mid = 0;
		gint64 volume_mid[2] = { 0, 0 };
		gint64 skip_paths[AXIS_ESTIMATE_N_REASONS];
		guint i;

		memset(skip_paths, 0, sizeof(skip_paths));
		g_assert_nonnull(total);
		for (i = 0; i < results->len; i++) {
			const AxisEstimateResult *r = &g_array_index(results, AxisEstimateResult, i);
			gint64 id = g_array_index(files, gint64, i);

			if (axis_estimate_result_converts(r)) {
				g_autofree gchar *volume = axis_test_library_q(fx->t, "SELECT volume_id FROM library_file WHERE id = %"
					G_GINT64_FORMAT, id);

				paths++;
				bytes += r->now;
				mid += r->mid;
				volume_mid[g_ascii_strtoll(volume, NULL, 10) == fx->s.vol_a ? 0 : 1] += r->mid;
			} else {
				skip_paths[r->reason]++;
			}
		}
		/* The middle total is the sum of the files' middles, exactly. */
		g_assert_cmpint(total->paths, ==, paths);
		g_assert_cmpint(total->bytes, ==, bytes);
		g_assert_cmpint(total->mid, ==, mid);
		g_assert_cmpint(total->low, <=, total->mid);
		g_assert_cmpint(total->mid, <=, total->high);
		g_assert_cmpint(total->high, <=, total->bytes);
		{
			g_autofree gchar *a = g_strdup_printf("%" G_GINT64_FORMAT, fx->s.vol_a);
			g_autofree gchar *b = g_strdup_printf("%" G_GINT64_FORMAT, fx->s.vol_b);
			const AxisEstimateFact *va = axis_estimate_snapshot_fact(snap, targets[t], "volume", a);
			const AxisEstimateFact *vb = axis_estimate_snapshot_fact(snap, targets[t], "volume", b);

			g_assert_nonnull(va);
			g_assert_nonnull(vb);
			g_assert_cmpint(va->mid, ==, volume_mid[0]);
			g_assert_cmpint(vb->mid, ==, volume_mid[1]);
			g_assert_cmpint(va->mid + vb->mid, ==, total->mid);
		}
		for (i = 1; i < AXIS_ESTIMATE_N_REASONS; i++) {
			const AxisEstimateFact *skip = axis_estimate_snapshot_fact(snap, targets[t], "skip",
				axis_estimate_reason_to_string((AxisEstimateReason)i));

			if (i == AXIS_ESTIMATE_NOT_APPLICABLE || i == AXIS_ESTIMATE_OFFLINE) {
				g_assert_null(skip);
				continue;
			}
			g_assert_cmpint(skip != NULL ? skip->paths : 0, ==, skip_paths[i]);
		}
	}

	/* Space saver converts Big, Remux, HDR10, the episode and Other. */
	g_assert_cmpint(axis_estimate_snapshot_fact(snap, "space_saver", "total", "")->paths, ==, 5);
	/* The two hard links of Linked (2007) are two paths, one file's bytes. */
	{
		const AxisEstimateFact *pinned = axis_estimate_snapshot_fact(snap, "space_saver", "skip", "pinned");
		g_autofree gchar *size = axis_test_library_q(fx->t, "SELECT size FROM library_file WHERE id = %"
			G_GINT64_FORMAT, file_id(fx, fx->s.films_a, "Linked (2007)/Linked (2007).mkv"));

		g_assert_cmpint(pinned->paths, ==, 2);
		g_assert_cmpint(pinned->bytes, ==, g_ascii_strtoll(size, NULL, 10));
	}
	/* The album with a lossy track: two files skipped, for two reasons. */
	g_assert_cmpint(axis_estimate_snapshot_fact(snap, "music_space_saver", "skip", "album")->paths, ==, 1);
	g_assert_cmpint(axis_estimate_snapshot_fact(snap, "music_space_saver", "skip", "lossy_source")->paths, ==, 1);
	/* Uncalibrated, and the coverage counts what could not be seen. */
	g_assert_cmpstr(json_object_get_string_member(json_object_get_object_member(json_object_get_object_member(
		snap->config, "targets"), "space_saver"), "calibration"), ==, "none");
	g_assert_cmpint(axis_analysis_extra_int(snap->coverage, "not_probed"), ==, 1);
	g_assert_cmpint(axis_analysis_extra_int(snap->coverage, "probe_failed"), ==, 1);
	g_assert_cmpint(axis_analysis_extra_int(snap->coverage, "hdr_uncertain"), ==, 1);
	/* The top list holds the files that convert, largest saving first. */
	{
		const AxisEstimateFact *top = axis_estimate_snapshot_fact(snap, "space_saver", "top", "");
		JsonArray *items = json_object_get_array_member(top->extra, "items");
		gint64 previous = G_MAXINT64;
		guint i;

		g_assert_cmpuint(json_array_get_length(items), ==, 5);
		for (i = 0; i < json_array_get_length(items); i++) {
			JsonObject *item = json_array_get_object_element(items, i);
			gint64 saving = json_object_get_int_member(item, "now") - json_object_get_int_member(item, "mid");
			AxisEstimateResult r = decide(fx, json_object_get_int_member(item, "id"), "space_saver");

			g_assert_cmpint(saving, <=, previous);
			g_assert_true(axis_estimate_result_converts(&r));
			g_assert_cmpint(r.mid, ==, json_object_get_int_member(item, "mid"));
			previous = saving;
		}
	}
}

/* A volume going away: totals stay, the volume and its files are marked. */
static void
test_offline_volume(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisEstimateSnapshot) before = NULL;
	g_autoptr(AxisEstimateSnapshot) after = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *b = NULL;
	const AxisEstimateFact *x;
	const AxisEstimateFact *y;

	SKIP_WITHOUT_DB(fx);
	refresh(fx);
	before = snapshot(fx);
	axis_test_library_unplug(fx->t, fx->s.disk_b, "away-b");
	axis_storage_check_volume(fx->t->storage, fx->s.vol_b, NULL, NULL);
	refresh(fx);
	after = snapshot(fx);

	x = axis_estimate_snapshot_fact(before, "space_saver", "total", "");
	y = axis_estimate_snapshot_fact(after, "space_saver", "total", "");
	g_assert_cmpint(x->paths, ==, y->paths);
	g_assert_cmpint(x->bytes, ==, y->bytes);
	g_assert_cmpint(x->mid, ==, y->mid);
	g_assert_cmpint(axis_analysis_extra_int(x->extra, "offline_paths"), ==, 0);
	g_assert_cmpint(axis_analysis_extra_int(y->extra, "offline_paths"), ==, 1);
	b = g_strdup_printf("%" G_GINT64_FORMAT, fx->s.vol_b);
	g_assert_false(json_object_get_boolean_member(axis_estimate_snapshot_fact(after, "space_saver", "volume",
		b)->extra, "usable"));
	g_assert_cmpint(axis_estimate_snapshot_fact(after, "space_saver", "volume", b)->paths, ==, 1);
	expect(fx, fx->s.films_b, "Other (2011)/Other (2011).mkv", "space_saver", AXIS_ESTIMATE_OFFLINE);
	{
		JsonArray *away = json_object_get_array_member(after->coverage, "volumes_unavailable");

		g_assert_cmpuint(json_array_get_length(away), ==, 1);
		g_assert_cmpint(json_object_get_int_member(json_array_get_object_element(away, 0), "paths"), ==, 1);
	}
	axis_test_library_plug(fx->t, fx->s.disk_b, "away-b");
	g_assert_true(axis_storage_check_volume(fx->t->storage, fx->s.vol_b, NULL, &error));
}

/* ── File lists ─────────────────────────────────────────────────── */

static gint64
list_count(
	Fixture     *fx,
	const gchar *query
){
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisFileList) list = NULL;
	AxisLibraryFilter filter;

	g_assert_true(axis_library_filter_parse(query, &filter, &error));
	g_assert_true(axis_estimator_list_files(fx->estimator, &filter, &list, &error));
	g_assert_no_error(error);
	g_assert_false(list->count_unknown);
	axis_library_filter_clear(&filter);
	return (gint64)list->count;
}

static void
test_lists_agree(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisEstimateSnapshot) snap = NULL;
	guint followed = 0;
	guint i;

	SKIP_WITHOUT_DB(fx);
	refresh(fx);
	snap = snapshot(fx);
	for (i = 0; i < snap->facts->len; i++) {
		const AxisEstimateFact *fact = g_ptr_array_index(snap->facts, i);
		g_autofree gchar *query = NULL;

		if (strcmp(fact->report, "skip") == 0)
			query = g_strdup_printf("target=%s&outcome=%s", fact->target, fact->dim);
		else if (strcmp(fact->report, "total") == 0)
			query = g_strdup_printf("target=%s&outcome=eligible", fact->target);
		else if (strcmp(fact->report, "volume") == 0)
			query = g_strdup_printf("target=%s&outcome=eligible&volume=%s", fact->target, fact->dim);
		else if (strcmp(fact->report, "root") == 0)
			query = g_strdup_printf("target=%s&outcome=eligible&root=%s", fact->target, fact->dim);
		else if (strcmp(fact->report, "category") == 0)
			query = g_strdup_printf("target=%s&outcome=eligible&type=%s", fact->target, fact->dim);
		else if (strcmp(fact->report, "res") == 0)
			query = g_strdup_printf("target=%s&outcome=eligible&res=%s", fact->target, fact->dim);
		else if (strcmp(fact->report, "vcodec") == 0)
			query = g_strdup_printf("target=%s&outcome=eligible&vcodec=%s", fact->target, fact->dim);
		else if (strcmp(fact->report, "acodec") == 0)
			query = g_strdup_printf("target=%s&outcome=eligible&acodec=%s", fact->target, fact->dim);
		if (query == NULL)
			continue;
		if (list_count(fx, query) != fact->paths)
			g_error("%s: the figure says %" G_GINT64_FORMAT ", the list %" G_GINT64_FORMAT, query, fact->paths,
				list_count(fx, query));
		followed++;
	}
	g_assert_cmpuint(followed, >, 40);

	/* "skipped" is everything the target is for that does not convert. */
	{
		const AxisEstimateFact *total = axis_estimate_snapshot_fact(snap, "space_saver", "total", "");

		g_assert_cmpint(list_count(fx, "target=space_saver&outcome=skipped"), ==,
			axis_analysis_extra_int(total->extra, "scope_paths") - total->paths);
		g_assert_cmpint(list_count(fx, "target=space_saver"), ==, axis_analysis_extra_int(total->extra, "scope_paths"));
	}

	/* The ordinary list refuses a filter it cannot evaluate. */
	{
		g_autoptr(GError) error = NULL;
		g_autoptr(AxisFileList) list = NULL;
		AxisLibraryFilter filter;

		g_assert_true(axis_library_filter_parse("target=space_saver&outcome=eligible", &filter, &error));
		g_assert_false(axis_library_list_files(fx->t->library, &filter, &list, &error));
		g_assert_error(error, AXIS_LIBRARY_FILTER_ERROR, AXIS_LIBRARY_FILTER_ERROR_INVALID);
		axis_library_filter_clear(&filter);
	}
}

/* Paging through an estimator's list visits every file once. */
static void
test_list_pages(Fixture *fx, gconstpointer data)
{
	g_autoptr(GHashTable) seen = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	g_autofree gchar *query = g_strdup("target=space_saver&sort=size");
	guint pages = 0;
	guint i;

	SKIP_WITHOUT_DB(fx);
	/* Enough files that a page fills: copies of Big on disk B. */
	for (i = 0; i < 130; i++) {
		g_autofree gchar *rel = g_strdup_printf("Copy %03u (2020)/Copy %03u (2020).mkv", i, i);

		axis_test_scenario_put(fx->s.disk_b, "Films2", rel, 2000 + i);
	}
	axis_scan_result_free(axis_test_library_scan_expect(fx->t, fx->s.films_b, AXIS_SCAN_COMPLETE,
		AXIS_SCAN_REASON_NONE));
	for (;;) {
		g_autoptr(GError) error = NULL;
		g_autoptr(AxisFileList) list = NULL;
		AxisLibraryFilter filter;

		g_assert_true(axis_library_filter_parse(query, &filter, &error));
		g_assert_true(axis_estimator_list_files(fx->estimator, &filter, &list, &error));
		g_assert_no_error(error);
		axis_library_filter_clear(&filter);
		for (i = 0; i < list->files->len; i++) {
			const AxisBrowseFile *file = g_ptr_array_index(list->files, i);

			g_assert_false(g_hash_table_contains(seen, &file->id));
			g_hash_table_add(seen, g_memdup2(&file->id, sizeof(gint64)));
		}
		pages++;
		if (pages == 1)
			g_assert_cmpuint(list->count, ==, 130 + 14);
		if (list->next == 0)
			break;
		g_free(query);
		query = g_strdup_printf("target=space_saver&sort=size&after=%" G_GINT64_FORMAT, list->next);
	}
	g_assert_cmpuint(pages, ==, 2);
	/* The video files of the video roots: 14 of the scenario, 130 new. */
	g_assert_cmpuint(g_hash_table_size(seen), ==, 130 + 14);
}

/* ── Calibration ────────────────────────────────────────────────── */

static void
test_calibration(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisEstimateSnapshot) snap = NULL;
	AxisEstimateResult before;
	AxisEstimateResult after;
	AxisEstimateSample sample;
	gint64 big;
	guint i;

	SKIP_WITHOUT_DB(fx);
	big = file_id(fx, fx->s.films_a, "Big (2001)/Big (2001).mkv");
	before = decide(fx, big, "space_saver");

	memset(&sample, 0, sizeof(sample));
	sample.target = "space_saver";
	sample.codec = AXIS_TCODEC_HEVC;
	sample.family = AXIS_ENCODER_SOFTWARE;
	sample.quality = 4;
	sample.part = "video";
	sample.source_codec = "h264";
	sample.width = 1920;
	sample.height = 1080;
	sample.frame_rate = 24.0;
	sample.source_bitrate = 8000000;
	sample.seconds = 60.0;
	sample.output_bitrate = 1000000;
	/* A sample the model cannot use is refused. */
	sample.source_codec = "prores";
	g_assert_false(axis_estimator_record_sample(fx->estimator, NULL, &sample, "measured", 0, big, &error));
	g_assert_error(error, AXIS_ESTIMATE_ERROR, AXIS_ESTIMATE_ERROR_FAILED);
	g_clear_error(&error);
	sample.source_codec = "h264";
	g_assert_false(axis_estimator_record_sample(fx->estimator, NULL, &sample, "guessed", 0, big, &error));
	g_clear_error(&error);

	for (i = 0; i < AXIS_ESTIMATE_CALIBRATED_SAMPLES; i++)
		g_assert_true(axis_estimator_record_sample(fx->estimator, NULL, &sample, "measured", 0, big, &error));
	g_assert_no_error(error);
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM estimate_sample"), ==,
		AXIS_ESTIMATE_CALIBRATED_SAMPLES);

	/* Results smaller than estimated make later estimates smaller. */
	after = decide(fx, big, "space_saver");
	g_assert_cmpint(after.mid, <, before.mid);
	{
		g_autoptr(GArray) estimates = NULL;

		g_assert_true(axis_estimator_estimate_file(fx->estimator, big, &estimates, &error));
		g_assert_cmpint(g_array_index(estimates, AxisFileEstimate, 0).state, ==, AXIS_CALIBRATION_FULL);
		g_assert_cmpuint(g_array_index(estimates, AxisFileEstimate, 0).samples, ==, AXIS_ESTIMATE_CALIBRATED_SAMPLES);
	}

	/* A snapshot states it per target: Space saver has a calibrated cell
	 * and uncalibrated ones in use; Balanced shares the HEVC cells (its
	 * 8-bit factor is part of the prior); Maximum compatibility (H.264) is
	 * still uncalibrated. */
	refresh(fx);
	snap = snapshot(fx);
	g_assert_cmpstr(json_object_get_string_member(json_object_get_object_member(json_object_get_object_member(
		snap->config, "targets"), "space_saver"), "calibration"), ==, "partly");
	g_assert_cmpstr(json_object_get_string_member(json_object_get_object_member(json_object_get_object_member(
		snap->config, "targets"), "balanced"), "calibration"), ==, "partly");
	g_assert_cmpstr(json_object_get_string_member(json_object_get_object_member(json_object_get_object_member(
		snap->config, "targets"), "max_compat"), "calibration"), ==, "none");
	/* A sample keeps its facts when its file goes. */
	axis_test_db_exec(fx->t->db, "UPDATE estimate_sample SET file_id = NULL");
}

/* ── When a refresh is due ──────────────────────────────────────── */

static gboolean
due(Fixture *fx)
{
	g_autoptr(GError) error = NULL;
	gboolean enqueued = FALSE;

	g_assert_true(axis_estimator_enqueue_due(fx->estimator, &enqueued, &error));
	g_assert_no_error(error);
	if (enqueued)
		axis_test_db_exec(fx->t->db, "DELETE FROM job WHERE kind = '" AXIS_ESTIMATE_REFRESH_KIND "'");
	return enqueued;
}

static void
test_due(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisAnalysis) analysis = NULL;
	g_autoptr(GError) error = NULL;
	AxisEstimateSample sample;

	SKIP_WITHOUT_DB(fx);
	/* Nothing before the first Analysis refresh. */
	g_assert_false(due(fx));
	analysis = axis_analysis_new(fx->t->pool, fx->settings);
	g_assert_true(axis_analysis_refresh(analysis, "direct", 0, NULL, NULL, &error));
	g_assert_true(due(fx));
	/* Not while an Analysis refresh waits: they follow each other. */
	g_assert_true(axis_analysis_request_refresh(analysis, NULL, &error));
	g_assert_false(due(fx));
	axis_test_db_exec(fx->t->db, "DELETE FROM job WHERE kind = 'analysis.refresh'");
	refresh(fx);
	g_assert_false(due(fx));
	/* A new sample, a new Analysis snapshot, a changed setting. */
	memset(&sample, 0, sizeof(sample));
	sample.target = "music_space_saver";
	sample.codec = AXIS_TCODEC_OPUS;
	sample.family = AXIS_ENCODER_SOFTWARE;
	sample.quality = 4;
	sample.part = "audio";
	sample.source_codec = "flac";
	sample.source_bitrate = 900000;
	sample.seconds = 20.0;
	sample.output_bitrate = 130000;
	g_assert_true(axis_estimator_record_sample(fx->estimator, NULL, &sample, "measured", 0, 0, &error));
	g_assert_true(due(fx));
	refresh(fx);
	g_assert_false(due(fx));
	g_assert_true(axis_analysis_refresh(analysis, "direct", 0, NULL, NULL, &error));
	g_assert_true(due(fx));
	refresh(fx);
	g_assert_false(due(fx));
	/* The one setting left that shapes the figures (the minimum saving
	 * and the 4K sizes are a profile's now). */
	g_assert_true(axis_settings_set(fx->settings, AXIS_ESTIMATE_SETTING_ENCODER, "nvenc", &error));
	g_assert_no_error(error);
	g_assert_true(due(fx));
	refresh(fx);
	g_assert_false(due(fx));
	/* A profile on schedule: a copy is not, until it is assigned; then an
	 * edit of it makes a refresh due, and its own minimum saving decides. */
	{
		g_autofree gchar *slug = NULL;
		AxisProfileDefinition def;
		AxisProfileSaved saved;

		g_assert_true(axis_profile_store_copy(fx->t->pool, "space_saver", "Picky", "root", &slug, &error));
		g_assert_no_error(error);
		g_assert_false(due(fx));
		g_assert_true(axis_profile_store_assign(fx->t->pool, fx->s.films_a, slug, "unknown", TRUE, "root", &error));
		g_assert_no_error(error);
		g_assert_true(due(fx));
		refresh(fx);
		g_assert_false(due(fx));
		/* The figures follow the profile: Big saves less than 90 %. */
		expect(fx, fx->s.films_a, "Big (2001)/Big (2001).mkv", slug, AXIS_ESTIMATE_ELIGIBLE);
		g_assert_true(axis_profile_definition_parse(axis_profile_builtin("space_saver")->definition, -1, &def, NULL));
		def.min_saving_percent = 90;
		g_assert_true(axis_profile_store_save(fx->t->pool, slug, 1, &def, "Picky", "", "root", &saved, NULL, &error));
		g_assert_no_error(error);
		g_assert_true(saved.created);
		g_assert_true(due(fx));
		expect(fx, fx->s.films_a, "Big (2001)/Big (2001).mkv", slug, AXIS_ESTIMATE_WOULD_NOT_SHRINK);
		expect(fx, fx->s.films_a, "Big (2001)/Big (2001).mkv", "space_saver", AXIS_ESTIMATE_ELIGIBLE);
	}
}

/* The refresh refuses a second one at the same time and a bad trigger. */
static void
test_refresh_refusals(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;

	SKIP_WITHOUT_DB(fx);
	g_assert_false(axis_estimator_refresh(fx->estimator, "whenever", 0, NULL, NULL, &error));
	g_assert_error(error, AXIS_ESTIMATE_ERROR, AXIS_ESTIMATE_ERROR_FAILED);
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM estimate_snapshot"), ==, 0);
}

#define ADD(name, func) g_test_add("/estimate/db/" name, Fixture, NULL, fixture_setup, func, fixture_teardown)

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	ADD("decisions", test_decisions);
	ADD("loader-agrees", test_loader_agrees);
	ADD("totals-are-sums", test_totals_are_sums);
	ADD("offline-volume", test_offline_volume);
	ADD("lists-agree", test_lists_agree);
	ADD("list-pages", test_list_pages);
	ADD("calibration", test_calibration);
	ADD("due", test_due);
	ADD("refresh-refusals", test_refresh_refusals);

	return g_test_run();
}
