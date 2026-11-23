/*
 * test-import-folder.c - the rules for removing a Usenet download's leftover
 * folder after its import (axis_import_folder_judge(), pure), and the
 * migration that has the imports recorded before them looked at once more
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Every title and release here is made up.  The removal itself, through a
 * real volume, the client and the hourly cleanup, is in test-import.c; the
 * write layer's removal of a folder in test-volume-write.c.
 *
 * Spec scenarios (import): A Usenet download's leftover folder is removed
 * after its import: each rule that leaves a folder; Imports recorded before
 * are looked at again; (fix-first-torrent) the job's folder of a reported
 * file and an import that took it as its source; a torrent gone from its
 * client, with a folder of Axis's own and with none; migration 0041.
 */

#include <glib.h>
#include <string.h>

#include "axis-test-db.h"
#include "db/axis-db-migrate.h"
#include "db/axis-db-pool.h"
#include "import/axis-import-private.h"

#define JOB        "Marrowfield.Lanterns.2011.1080p.BluRay.x264-MADEUP"
#define CLIENT     "/usenet/complete/axis/" JOB
#define CATEGORY   "/usenet/complete/axis"
#define PREFIX     "/usenet"
#define MAPPED     "downloads/complete/axis/" JOB
#define VOLUME     7

/* ── The rules ──────────────────────────────────────────────────── */

typedef struct
{
	AxisImportFolderFacts facts;
	AxisImportFolderRoot  films;
	AxisImportFolderRoot  downloads;
	GPtrArray            *roots;
	GPtrArray            *others;
} Case;

/* A folder every rule allows: the job's own folder inside the category's,
 * through a mapping that holds both, imported from that very place, in the
 * downloads root and beside another job. */
static void
case_init(Case *c)
{
	memset(c, 0, sizeof(*c));
	c->films.rel_path = "films";
	c->films.downloads = FALSE;
	c->films.name = "Films";
	c->downloads.rel_path = "downloads";
	c->downloads.downloads = TRUE;
	c->downloads.name = "Downloads";
	c->roots = g_ptr_array_new();
	g_ptr_array_add(c->roots, &c->films);
	g_ptr_array_add(c->roots, &c->downloads);
	c->others = g_ptr_array_new();
	g_ptr_array_add(c->others, (gpointer)"downloads/complete/axis/Quillfeather.Junction.2014-MADEUP");
	g_ptr_array_add(c->others, (gpointer)"downloads/complete/axis/" JOB ".1");
	c->facts.client_path = CLIENT;
	c->facts.category_folder = CATEGORY;
	c->facts.map_prefix = PREFIX;
	c->facts.mapped_volume = VOLUME;
	c->facts.mapped_path = MAPPED;
	c->facts.recorded_volume = VOLUME;
	c->facts.recorded_path = MAPPED;
	c->facts.source_volume = VOLUME;
	c->facts.source_path = MAPPED;
	c->facts.roots = c->roots;
	c->facts.others = c->others;
}

static void
case_clear(Case *c)
{
	g_ptr_array_unref(c->roots);
	g_ptr_array_unref(c->others);
}

/* The rule for what @shape is at the reported place, and that its words are
 * there (and contain @words when given); what goes, when allowed. */
static void
assert_shape_rule(
	const Case             *c,
	AxisImportFolderShape   shape,
	AxisImportFolderRule    expected,
	const gchar            *words,
	AxisImportFolderTarget *target_out
){
	g_autofree gchar *why = NULL;
	AxisImportFolderTarget target;
	AxisImportFolderRule rule = axis_import_folder_judge(&c->facts, shape, &target, &why);

	if (rule != expected)
		g_error("expected rule %d, got %d (%s)", (gint)expected, (gint)rule, why != NULL ? why : "no words");
	if (expected == AXIS_IMPORT_FOLDER_OK) {
		g_assert_null(why);
		g_assert_nonnull(target.path);
		g_assert_nonnull(target.client_path);
		g_assert_nonnull(target.inner);
		if (target_out != NULL)
			*target_out = target;
		else
			axis_import_folder_target_clear(&target);
		return;
	}
	g_assert_null(target.path);
	g_assert_nonnull(why);
	g_assert_cmpuint(strlen(why), >, 10);
	if (words != NULL && strstr(why, words) == NULL)
		g_error("expected \"%s\" in: %s", words, why);
}

/* As above, a folder at the reported place. */
static void
assert_rule(
	const Case           *c,
	AxisImportFolderRule  expected,
	const gchar          *words
){
	assert_shape_rule(c, AXIS_IMPORT_FOLDER_SHAPE_FOLDER, expected, words, NULL);
}

/* The job's own folder, inside the category's: removed. */
static void
test_allowed(void)
{
	Case c;

	case_init(&c);
	assert_rule(&c, AXIS_IMPORT_FOLDER_OK, NULL);
	/* As the client wrote it, with a trailing slash: the same folder. */
	c.facts.client_path = CLIENT "/";
	assert_rule(&c, AXIS_IMPORT_FOLDER_OK, NULL);
	/* A downloads root at the volume's top holds it: still allowed. */
	c.downloads.rel_path = "";
	g_ptr_array_remove(c.roots, &c.films);
	assert_rule(&c, AXIS_IMPORT_FOLDER_OK, NULL);
	/* The mapping is the category's folder itself. */
	c.facts.map_prefix = CATEGORY;
	assert_rule(&c, AXIS_IMPORT_FOLDER_OK, NULL);
	case_clear(&c);
}

/* Never a path with "." or "..", never a relative one: refused, never
 * resolved. */
static void
test_bad_paths(void)
{
	static const gchar *const paths[] = {
		"/usenet/complete/axis/../axis/" JOB, "/usenet/complete/axis/./" JOB, "/usenet/complete/axis/" JOB "/..",
		"usenet/complete/axis/" JOB, "", "/usenet/complete/axis/Marrowfield\x01Lanterns", NULL
	};
	Case c;
	guint i;

	case_init(&c);
	for (i = 0; paths[i] != NULL; i++) {
		g_test_message("%s", paths[i]);
		c.facts.client_path = paths[i];
		assert_rule(&c, AXIS_IMPORT_FOLDER_BAD_PATH, "'..'");
	}
	c.facts.client_path = NULL;
	assert_rule(&c, AXIS_IMPORT_FOLDER_BAD_PATH, NULL);
	case_clear(&c);
}

/* Never the category's folder itself (all of a job's files put straight
 * into it), never outside it. */
static void
test_category(void)
{
	Case c;

	case_init(&c);
	c.facts.client_path = CATEGORY;
	assert_rule(&c, AXIS_IMPORT_FOLDER_IS_CATEGORY, "itself");
	c.facts.client_path = CATEGORY "/";
	assert_rule(&c, AXIS_IMPORT_FOLDER_IS_CATEGORY, NULL);

	c.facts.client_path = CLIENT;
	c.facts.category_folder = "/usenet/complete/other";
	assert_rule(&c, AXIS_IMPORT_FOLDER_OUTSIDE_CATEGORY, "/usenet/complete/other");
	/* By whole parts: "axis" is not inside "ax". */
	c.facts.category_folder = "/usenet/complete/ax";
	assert_rule(&c, AXIS_IMPORT_FOLDER_OUTSIDE_CATEGORY, NULL);
	/* A folder above the category's (SABnzbd's own) is not it. */
	c.facts.category_folder = CATEGORY "/" JOB "/deeper";
	assert_rule(&c, AXIS_IMPORT_FOLDER_OUTSIDE_CATEGORY, NULL);
	c.facts.category_folder = NULL;
	assert_rule(&c, AXIS_IMPORT_FOLDER_OUTSIDE_CATEGORY, "not known");
	case_clear(&c);
}

/* The one mapping that reaches it must hold the category's folder too. */
static void
test_mapping(void)
{
	Case c;

	case_init(&c);
	c.facts.map_prefix = CLIENT;
	assert_rule(&c, AXIS_IMPORT_FOLDER_MAP_INSIDE, CLIENT);
	c.facts.map_prefix = CATEGORY "/Lantern";
	assert_rule(&c, AXIS_IMPORT_FOLDER_MAP_INSIDE, NULL);
	c.facts.map_prefix = NULL;
	assert_rule(&c, AXIS_IMPORT_FOLDER_MAP_INSIDE, NULL);
	case_clear(&c);
}

/* Through the mapping it is the very place the download was recorded at and
 * the import took its files from, on the same volume. */
static void
test_imported_from(void)
{
	Case c;

	case_init(&c);
	c.facts.mapped_path = "downloads/complete/axis/Bramblewick.Tides.2010-MADEUP";
	assert_rule(&c, AXIS_IMPORT_FOLDER_NOT_IMPORTED_FROM, "imported from");
	case_clear(&c);
	case_init(&c);
	c.facts.recorded_path = "downloads/elsewhere/" JOB;
	assert_rule(&c, AXIS_IMPORT_FOLDER_NOT_IMPORTED_FROM, NULL);
	case_clear(&c);
	case_init(&c);
	c.facts.source_path = "downloads/elsewhere/" JOB;
	assert_rule(&c, AXIS_IMPORT_FOLDER_NOT_IMPORTED_FROM, NULL);
	case_clear(&c);
	case_init(&c);
	c.facts.mapped_volume = VOLUME + 1;
	assert_rule(&c, AXIS_IMPORT_FOLDER_NOT_IMPORTED_FROM, NULL);
	case_clear(&c);
	case_init(&c);
	c.facts.source_volume = 0;
	assert_rule(&c, AXIS_IMPORT_FOLDER_NOT_IMPORTED_FROM, NULL);
	c.facts.source_volume = VOLUME;
	c.facts.mapped_path = NULL;
	assert_rule(&c, AXIS_IMPORT_FOLDER_NOT_IMPORTED_FROM, NULL);
	case_clear(&c);
}

/* Never the volume's top or a folder of Axis's own there. */
static void
test_axis_own(void)
{
	static const gchar *const places[] = {
		"", ".axis-recycle/" JOB, ".axis-work/" JOB, ".axis-recycle", NULL
	};
	Case c;
	guint i;

	for (i = 0; places[i] != NULL; i++) {
		case_init(&c);
		c.facts.mapped_path = c.facts.recorded_path = c.facts.source_path = places[i];
		assert_rule(&c, AXIS_IMPORT_FOLDER_AXIS_OWN, "Axis's own");
		case_clear(&c);
	}
}

/* Never anything inside a library root (a downloads root aside), never a
 * root or a folder that holds one. */
static void
test_library_roots(void)
{
	AxisImportFolderRoot inside = { "downloads/complete", FALSE, "Shelf of Films" };
	AxisImportFolderRoot top = { "", FALSE, "Whole Disk" };
	AxisImportFolderRoot held = { MAPPED "/Extras", TRUE, "Odd Downloads" };
	AxisImportFolderRoot itself = { MAPPED, TRUE, "Job Root" };
	Case c;

	case_init(&c);
	g_ptr_array_add(c.roots, &inside);
	assert_rule(&c, AXIS_IMPORT_FOLDER_IN_LIBRARY, "Shelf of Films");
	case_clear(&c);

	case_init(&c);
	g_ptr_array_add(c.roots, &top);
	assert_rule(&c, AXIS_IMPORT_FOLDER_IN_LIBRARY, "Whole Disk");
	case_clear(&c);

	/* A root inside it, even a downloads one. */
	case_init(&c);
	g_ptr_array_add(c.roots, &held);
	assert_rule(&c, AXIS_IMPORT_FOLDER_HOLDS_ROOT, "holds the library root Odd Downloads");
	case_clear(&c);

	case_init(&c);
	g_ptr_array_add(c.roots, &itself);
	assert_rule(&c, AXIS_IMPORT_FOLDER_HOLDS_ROOT, "Job Root itself");
	case_clear(&c);

	/* The downloads root is a library root of type Downloads: it is where
	 * downloads are, so a job inside it may go. */
	case_init(&c);
	c.downloads.rel_path = "downloads/complete/axis";
	assert_rule(&c, AXIS_IMPORT_FOLDER_OK, NULL);
	case_clear(&c);
}

/* Never while another download Axis knows, or an import still open, is in
 * it or around it. */
static void
test_shared(void)
{
	Case c;

	case_init(&c);
	g_ptr_array_add(c.others, (gpointer)MAPPED "/Second.Part");
	assert_rule(&c, AXIS_IMPORT_FOLDER_SHARED, "another download");
	case_clear(&c);

	case_init(&c);
	g_ptr_array_add(c.others, (gpointer)"downloads/complete/axis");
	assert_rule(&c, AXIS_IMPORT_FOLDER_SHARED, NULL);
	case_clear(&c);

	case_init(&c);
	g_ptr_array_add(c.others, (gpointer)MAPPED);
	assert_rule(&c, AXIS_IMPORT_FOLDER_SHARED, NULL);
	case_clear(&c);
}

/* ── A file at the reported place ───────────────────────────────── */

/* The reported place is @below the category's folder; on the volume that is
 * beneath downloads/complete/axis. */
static void
report_at(
	Case        *c,
	gchar      **client,
	gchar      **mapped,
	const gchar *below
){
	*client = g_strconcat(CATEGORY "/", below, NULL);
	*mapped = g_strconcat("downloads/complete/axis/", below, NULL);
	c->facts.client_path = *client;
	c->facts.mapped_path = c->facts.recorded_path = c->facts.source_path = *mapped;
}

/* A folder reported: it goes, as it is. */
static void
test_folder_reported(void)
{
	AxisImportFolderTarget target;
	Case c;

	case_init(&c);
	assert_shape_rule(&c, AXIS_IMPORT_FOLDER_SHAPE_FOLDER, AXIS_IMPORT_FOLDER_OK, NULL, &target);
	g_assert_false(target.file);
	g_assert_cmpstr(target.path, ==, MAPPED);
	g_assert_cmpstr(target.client_path, ==, CLIENT);
	g_assert_cmpstr(target.inner, ==, "");
	axis_import_folder_target_clear(&target);
	case_clear(&c);
}

/* A file inside the job's folder (as SABnzbd reports a job of one file), or
 * deeper in it: the job's folder goes, the first part beneath the
 * category's; the file is looked for at its place inside it. */
static void
test_file_in_the_job_folder(void)
{
	static const struct
	{
		const gchar *below;
		const gchar *inner;
	} cases[] = {
		{ JOB "/" JOB ".mkv", JOB ".mkv" },
		{ JOB "/Feature/" JOB ".mkv", "Feature/" JOB ".mkv" },
		{ JOB "/a/b/c/" JOB ".mkv", "a/b/c/" JOB ".mkv" }
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		g_autofree gchar *client = NULL;
		g_autofree gchar *mapped = NULL;
		AxisImportFolderTarget target;
		Case c;

		case_init(&c);
		report_at(&c, &client, &mapped, cases[i].below);
		assert_shape_rule(&c, AXIS_IMPORT_FOLDER_SHAPE_FILE, AXIS_IMPORT_FOLDER_OK, NULL, &target);
		g_assert_false(target.file);
		g_assert_cmpstr(target.path, ==, MAPPED);
		g_assert_cmpstr(target.client_path, ==, CLIENT);
		g_assert_cmpstr(target.inner, ==, cases[i].inner);
		axis_import_folder_target_clear(&target);
		case_clear(&c);
	}
}

/* A file straight in the category's folder: that file alone goes, never the
 * category's folder. */
static void
test_file_in_the_category_folder(void)
{
	g_autofree gchar *client = NULL;
	g_autofree gchar *mapped = NULL;
	AxisImportFolderTarget target;
	Case c;

	case_init(&c);
	report_at(&c, &client, &mapped, JOB ".mkv");
	assert_shape_rule(&c, AXIS_IMPORT_FOLDER_SHAPE_FILE, AXIS_IMPORT_FOLDER_OK, NULL, &target);
	g_assert_true(target.file);
	g_assert_cmpstr(target.path, ==, "downloads/complete/axis/" JOB ".mkv");
	g_assert_cmpstr(target.client_path, ==, CATEGORY "/" JOB ".mkv");
	axis_import_folder_target_clear(&target);
	/* As a folder at the same place, it is the job's folder. */
	assert_shape_rule(&c, AXIS_IMPORT_FOLDER_SHAPE_FOLDER, AXIS_IMPORT_FOLDER_OK, NULL, &target);
	g_assert_false(target.file);
	axis_import_folder_target_clear(&target);
	case_clear(&c);
}

/* For a reported file the rules of the place are the job folder's: a root
 * that is it or is in it, another download in it beside the file. */
static void
test_file_rules_are_the_job_folders(void)
{
	AxisImportFolderRoot job_root = { MAPPED, TRUE, "Job Root" };
	AxisImportFolderRoot inner_root = { MAPPED "/Extras", TRUE, "Inner Root" };
	AxisImportFolderRoot films = { "downloads/complete/axis/" JOB, FALSE, "Shelf of Films" };
	g_autofree gchar *client = NULL;
	g_autofree gchar *mapped = NULL;
	Case c;

	case_init(&c);
	report_at(&c, &client, &mapped, JOB "/" JOB ".mkv");
	g_ptr_array_add(c.roots, &job_root);
	assert_shape_rule(&c, AXIS_IMPORT_FOLDER_SHAPE_FILE, AXIS_IMPORT_FOLDER_HOLDS_ROOT, "Job Root itself", NULL);
	g_ptr_array_remove(c.roots, &job_root);
	g_ptr_array_add(c.roots, &inner_root);
	assert_shape_rule(&c, AXIS_IMPORT_FOLDER_SHAPE_FILE, AXIS_IMPORT_FOLDER_HOLDS_ROOT, "Inner Root", NULL);
	g_ptr_array_remove(c.roots, &inner_root);
	g_ptr_array_add(c.roots, &films);
	assert_shape_rule(&c, AXIS_IMPORT_FOLDER_SHAPE_FILE, AXIS_IMPORT_FOLDER_HOLDS_ROOT, NULL, NULL);
	g_ptr_array_remove(c.roots, &films);
	/* Another download in the job's folder, beside the file, not at it. */
	g_ptr_array_add(c.others, (gpointer)MAPPED "/Second.Part");
	assert_shape_rule(&c, AXIS_IMPORT_FOLDER_SHAPE_FILE, AXIS_IMPORT_FOLDER_SHARED, "another download", NULL);
	/* As a folder at the file's place (nothing of that sort is beside it), it would go. */
	assert_shape_rule(&c, AXIS_IMPORT_FOLDER_SHAPE_FOLDER, AXIS_IMPORT_FOLDER_OK, NULL, NULL);
	case_clear(&c);

	/* A file straight in the category's folder, inside a root of films. */
	{
		AxisImportFolderRoot category_films = { "downloads/complete/axis", FALSE, "Category Films" };

		case_init(&c);
		g_clear_pointer(&client, g_free);
		g_clear_pointer(&mapped, g_free);
		report_at(&c, &client, &mapped, JOB ".mkv");
		g_ptr_array_add(c.roots, &category_films);
		assert_shape_rule(&c, AXIS_IMPORT_FOLDER_SHAPE_FILE, AXIS_IMPORT_FOLDER_IN_LIBRARY, "Category Films", NULL);
		case_clear(&c);
	}
}

/* A volume place that does not end as the client's does: the job's folder
 * cannot be told on the volume. */
static void
test_file_place_that_does_not_match(void)
{
	Case c;

	case_init(&c);
	c.facts.client_path = CLIENT "/" JOB ".mkv";
	c.facts.mapped_path = c.facts.recorded_path = c.facts.source_path = "elsewhere.mkv";
	assert_shape_rule(&c, AXIS_IMPORT_FOLDER_SHAPE_FILE, AXIS_IMPORT_FOLDER_NOT_IMPORTED_FROM, "cannot be told", NULL);
	c.facts.mapped_path = c.facts.recorded_path = c.facts.source_path = "downloads/other/" JOB ".mkv";
	assert_shape_rule(&c, AXIS_IMPORT_FOLDER_SHAPE_FILE, AXIS_IMPORT_FOLDER_OK, NULL, NULL);
	case_clear(&c);
}

/* ── The job's folder of a reported file (fix-first-torrent) ─────── */

static void
test_job_folder(void)
{
	g_autofree gchar *job_client = NULL;
	g_autofree gchar *job_rel = NULL;
	g_autofree gchar *inner = NULL;

	g_assert_cmpint(axis_import_job_folder(CATEGORY, CLIENT "/" JOB ".mkv", MAPPED "/" JOB ".mkv", &job_client, &job_rel,
		&inner), ==, AXIS_IMPORT_JOB_FOLDER);
	g_assert_cmpstr(job_client, ==, CLIENT);
	g_assert_cmpstr(job_rel, ==, MAPPED);
	g_assert_cmpstr(inner, ==, JOB ".mkv");
	g_clear_pointer(&job_client, g_free);
	g_clear_pointer(&job_rel, g_free);
	g_clear_pointer(&inner, g_free);
	/* Deeper: still the first part beneath the category's folder. */
	g_assert_cmpint(axis_import_job_folder(CATEGORY, CLIENT "/a/b/" JOB ".mkv", MAPPED "/a/b/" JOB ".mkv", NULL, &job_rel,
		&inner), ==, AXIS_IMPORT_JOB_FOLDER);
	g_assert_cmpstr(job_rel, ==, MAPPED);
	g_assert_cmpstr(inner, ==, "a/b/" JOB ".mkv");
	g_clear_pointer(&job_rel, g_free);
	/* Straight in the category's folder: no job's folder. */
	g_assert_cmpint(axis_import_job_folder(CATEGORY, CATEGORY "/" JOB ".mkv", "downloads/complete/axis/" JOB ".mkv",
		NULL, &job_rel, NULL), ==, AXIS_IMPORT_JOB_NONE);
	g_assert_null(job_rel);
	/* The category's folder itself, outside it, a volume place that does not
	 * end as the client's path: it cannot be told. */
	g_assert_cmpint(axis_import_job_folder(CATEGORY, CATEGORY, MAPPED, NULL, &job_rel, NULL), ==,
		AXIS_IMPORT_JOB_UNTOLD);
	g_assert_cmpint(axis_import_job_folder(CATEGORY, "/usenet/complete/other/" JOB "/" JOB ".mkv", MAPPED, NULL, &job_rel,
		NULL), ==, AXIS_IMPORT_JOB_UNTOLD);
	g_assert_cmpint(axis_import_job_folder(CATEGORY, CLIENT "/" JOB ".mkv", "elsewhere.mkv", NULL, &job_rel, NULL), ==,
		AXIS_IMPORT_JOB_UNTOLD);
	g_assert_cmpint(axis_import_job_folder(CATEGORY, CLIENT "/" JOB ".mkv", "x" JOB ".mkv", NULL, &job_rel, NULL), ==,
		AXIS_IMPORT_JOB_UNTOLD);
	g_assert_cmpint(axis_import_job_folder(NULL, CLIENT "/" JOB ".mkv", MAPPED "/" JOB ".mkv", NULL, &job_rel, NULL), ==,
		AXIS_IMPORT_JOB_UNTOLD);
	g_assert_null(job_rel);
}

/* The import took its files from the job's folder around the reported file:
 * that folder goes, and the import's files are looked for in it as they were
 * recorded, relative to it.  Any other source is not the place imported from. */
static void
test_source_is_the_job_folder(void)
{
	g_autofree gchar *client = NULL;
	g_autofree gchar *mapped = NULL;
	AxisImportFolderTarget target;
	Case c;

	case_init(&c);
	report_at(&c, &client, &mapped, JOB "/Feature/" JOB ".mkv");
	c.facts.source_path = MAPPED;
	assert_shape_rule(&c, AXIS_IMPORT_FOLDER_SHAPE_FILE, AXIS_IMPORT_FOLDER_OK, NULL, &target);
	g_assert_false(target.file);
	g_assert_cmpstr(target.path, ==, MAPPED);
	g_assert_cmpstr(target.client_path, ==, CLIENT);
	g_assert_cmpstr(target.inner, ==, "");
	axis_import_folder_target_clear(&target);
	/* A folder at the reported place: its own source only. */
	assert_shape_rule(&c, AXIS_IMPORT_FOLDER_SHAPE_FOLDER, AXIS_IMPORT_FOLDER_NOT_IMPORTED_FROM, "imported from", NULL);
	/* A folder between the job's and the file, or around the category's. */
	c.facts.source_path = MAPPED "/Feature";
	assert_shape_rule(&c, AXIS_IMPORT_FOLDER_SHAPE_FILE, AXIS_IMPORT_FOLDER_NOT_IMPORTED_FROM, "imported from", NULL);
	c.facts.source_path = "downloads/complete/axis";
	assert_shape_rule(&c, AXIS_IMPORT_FOLDER_SHAPE_FILE, AXIS_IMPORT_FOLDER_NOT_IMPORTED_FROM, NULL, NULL);
	case_clear(&c);

	/* A file straight in the category's folder has no job's folder. */
	g_clear_pointer(&client, g_free);
	g_clear_pointer(&mapped, g_free);
	case_init(&c);
	report_at(&c, &client, &mapped, JOB ".mkv");
	c.facts.source_path = "downloads/complete/axis";
	assert_shape_rule(&c, AXIS_IMPORT_FOLDER_SHAPE_FILE, AXIS_IMPORT_FOLDER_NOT_IMPORTED_FROM, NULL, NULL);
	case_clear(&c);
}

/* A torrent its client no longer has: with the client's folder for Axis's
 * label, judged as a Usenet job is. */
static void
test_torrent_with_a_label_folder(void)
{
	AxisImportFolderTarget target;
	Case c;

	case_init(&c);
	c.facts.torrent = TRUE;
	assert_shape_rule(&c, AXIS_IMPORT_FOLDER_SHAPE_FOLDER, AXIS_IMPORT_FOLDER_OK, NULL, &target);
	g_assert_false(target.file);
	g_assert_false(target.loose);
	g_assert_cmpstr(target.path, ==, MAPPED);
	axis_import_folder_target_clear(&target);
	c.facts.category_folder = "/usenet/complete/other";
	assert_rule(&c, AXIS_IMPORT_FOLDER_OUTSIDE_CATEGORY, "not inside the folder of Axis's category");
	case_clear(&c);
}

/* With no folder of Axis's own: a torrent's folder never goes, the reason
 * saying where it is; its one file goes alone, under every other rule. */
static void
test_torrent_with_no_own_folder(void)
{
	AxisImportFolderRoot films = { "downloads", FALSE, "Shelf of Films" };
	AxisImportFolderTarget target;
	Case c;

	case_init(&c);
	c.facts.torrent = TRUE;
	c.facts.category_folder = NULL;
	c.facts.map_prefix = "/usenet";
	c.facts.no_folder_why = "the label \"axis\" has no folder of its own";
	assert_shape_rule(&c, AXIS_IMPORT_FOLDER_SHAPE_FOLDER, AXIS_IMPORT_FOLDER_NO_OWN_FOLDER,
		"no folder of their own (the label \"axis\" has no folder of its own)", NULL);
	assert_shape_rule(&c, AXIS_IMPORT_FOLDER_SHAPE_FOLDER, AXIS_IMPORT_FOLDER_NO_OWN_FOLDER, "It is at " CLIENT " at the "
		"client", NULL);
	/* Its one file: alone, the library's copy to be looked for first. */
	assert_shape_rule(&c, AXIS_IMPORT_FOLDER_SHAPE_FILE, AXIS_IMPORT_FOLDER_OK, NULL, &target);
	g_assert_true(target.file);
	g_assert_true(target.loose);
	g_assert_cmpstr(target.path, ==, MAPPED);
	g_assert_cmpstr(target.client_path, ==, CLIENT);
	axis_import_folder_target_clear(&target);
	/* Every other rule still holds for it. */
	c.facts.source_path = "downloads/elsewhere.mkv";
	assert_shape_rule(&c, AXIS_IMPORT_FOLDER_SHAPE_FILE, AXIS_IMPORT_FOLDER_NOT_IMPORTED_FROM, NULL, NULL);
	c.facts.source_path = MAPPED;
	g_ptr_array_add(c.others, (gpointer)MAPPED);
	assert_shape_rule(&c, AXIS_IMPORT_FOLDER_SHAPE_FILE, AXIS_IMPORT_FOLDER_SHARED, NULL, NULL);
	g_ptr_array_remove(c.others, (gpointer)MAPPED);
	c.films = films;
	assert_shape_rule(&c, AXIS_IMPORT_FOLDER_SHAPE_FILE, AXIS_IMPORT_FOLDER_IN_LIBRARY, "Shelf of Films", NULL);
	case_clear(&c);
	case_init(&c);
	c.facts.torrent = TRUE;
	c.facts.category_folder = NULL;
	c.facts.client_path = "/usenet/complete/../axis/" JOB;
	assert_shape_rule(&c, AXIS_IMPORT_FOLDER_SHAPE_FILE, AXIS_IMPORT_FOLDER_BAD_PATH, NULL, NULL);
	case_clear(&c);
	/* A Usenet job with no category folder known is refused as before. */
	case_init(&c);
	c.facts.category_folder = NULL;
	assert_shape_rule(&c, AXIS_IMPORT_FOLDER_SHAPE_FILE, AXIS_IMPORT_FOLDER_OUTSIDE_CATEGORY, "not known", NULL);
	case_clear(&c);
}

/* "Delete this download" for a torrent its client still has, which the
 * client is to delete (add-replace-grab-and-delete-download, slice B): the
 * one rule left out is the one about a folder of Axis's own; what goes is
 * the reported place itself; and every rule of the place stands, so that a
 * torrent in a library folder is refused before the client is asked. */
static void
test_delete_download_rules(void)
{
	AxisImportFolderRoot library = { "downloads", FALSE, "Shelf of Films" };
	AxisImportFolderRoot inside = { MAPPED "/Extras", TRUE, "A root inside it" };
	AxisImportFolderRoot itself = { MAPPED, TRUE, "A root at it" };
	AxisImportFolderTarget target;
	Case c;

	/* With no folder of Axis's own at the client: its folder may go (the
	 * client deletes it), where Axis itself would never delete it. */
	case_init(&c);
	c.facts.torrent = TRUE;
	c.facts.category_folder = NULL;
	assert_shape_rule(&c, AXIS_IMPORT_FOLDER_SHAPE_FOLDER, AXIS_IMPORT_FOLDER_NO_OWN_FOLDER, NULL, NULL);
	c.facts.client_deletes = TRUE;
	assert_shape_rule(&c, AXIS_IMPORT_FOLDER_SHAPE_FOLDER, AXIS_IMPORT_FOLDER_OK, NULL, &target);
	g_assert_false(target.file);
	g_assert_false(target.loose);
	g_assert_cmpstr(target.path, ==, MAPPED);
	g_assert_cmpstr(target.client_path, ==, CLIENT);
	axis_import_folder_target_clear(&target);
	assert_shape_rule(&c, AXIS_IMPORT_FOLDER_SHAPE_FILE, AXIS_IMPORT_FOLDER_OK, NULL, &target);
	g_assert_true(target.file);
	g_assert_false(target.loose);
	g_assert_cmpstr(target.path, ==, MAPPED);
	axis_import_folder_target_clear(&target);

	/* Inside a library folder; a library folder itself; one that holds a
	 * library folder. */
	c.films = library;
	assert_rule(&c, AXIS_IMPORT_FOLDER_IN_LIBRARY, "it is inside the library root Shelf of Films");
	assert_shape_rule(&c, AXIS_IMPORT_FOLDER_SHAPE_FILE, AXIS_IMPORT_FOLDER_IN_LIBRARY, "Shelf of Films", NULL);
	c.films.rel_path = "films";
	c.films.name = "Films";
	g_ptr_array_add(c.roots, &itself);
	assert_rule(&c, AXIS_IMPORT_FOLDER_HOLDS_ROOT, "it is the library root A root at it itself");
	g_ptr_array_remove(c.roots, &itself);
	g_ptr_array_add(c.roots, &inside);
	assert_rule(&c, AXIS_IMPORT_FOLDER_HOLDS_ROOT, "it holds the library root A root inside it");
	g_ptr_array_remove(c.roots, &inside);
	/* Another download in it, or around it. */
	g_ptr_array_add(c.others, (gpointer)MAPPED "/another");
	assert_rule(&c, AXIS_IMPORT_FOLDER_SHARED, NULL);
	g_ptr_array_remove(c.others, (gpointer)MAPPED "/another");
	g_ptr_array_add(c.others, (gpointer)"downloads/complete");
	assert_rule(&c, AXIS_IMPORT_FOLDER_SHARED, NULL);
	g_ptr_array_remove(c.others, (gpointer)"downloads/complete");
	/* Not the place recorded when it completed; a path with a ".." part. */
	c.facts.recorded_path = "downloads/complete/axis/Elsewhere";
	assert_rule(&c, AXIS_IMPORT_FOLDER_NOT_IMPORTED_FROM, NULL);
	c.facts.recorded_path = MAPPED;
	c.facts.client_path = "/usenet/complete/../axis/" JOB;
	assert_rule(&c, AXIS_IMPORT_FOLDER_BAD_PATH, NULL);
	c.facts.client_path = CLIENT;
	/* The volume's top, or a folder of Axis's own there. */
	c.facts.mapped_path = ".axis-recycle/" JOB;
	c.facts.recorded_path = ".axis-recycle/" JOB;
	c.facts.source_path = ".axis-recycle/" JOB;
	assert_rule(&c, AXIS_IMPORT_FOLDER_AXIS_OWN, NULL);
	case_clear(&c);

	/* With the client's folder for Axis's label: its rules stand too (never
	 * that folder itself, never outside it). */
	case_init(&c);
	c.facts.torrent = TRUE;
	c.facts.client_deletes = TRUE;
	assert_rule(&c, AXIS_IMPORT_FOLDER_OK, NULL);
	c.facts.category_folder = CLIENT;
	assert_rule(&c, AXIS_IMPORT_FOLDER_IS_CATEGORY, NULL);
	c.facts.category_folder = "/usenet/complete/other";
	assert_rule(&c, AXIS_IMPORT_FOLDER_OUTSIDE_CATEGORY, NULL);
	case_clear(&c);
}

/* ── The migration ──────────────────────────────────────────────── */

static gint
compare_names(
	gconstpointer a,
	gconstpointer b
){
	return g_strcmp0(*(gchar *const *)a, *(gchar *const *)b);
}

static AxisDbMigration *
migrations_up_to(
	guint        last,
	GPtrArray  **owned
){
	g_autoptr(GDir) dir = g_dir_open("data/migrations", 0, NULL);
	g_autoptr(GPtrArray) names = g_ptr_array_new_with_free_func(g_free);
	AxisDbMigration *list = g_new0(AxisDbMigration, last);
	const gchar *name;
	guint i;

	g_assert_nonnull(dir);
	while ((name = g_dir_read_name(dir)) != NULL)
		g_ptr_array_add(names, g_strdup(name));
	g_ptr_array_sort(names, compare_names);
	*owned = g_ptr_array_new_with_free_func(g_free);
	for (i = 0; i < last; i++) {
		const gchar *file = g_ptr_array_index(names, i);
		g_autofree gchar *path = g_build_filename("data/migrations", file, NULL);
		gchar *sql = NULL;
		gchar *migration_name = NULL;
		guint version = 0;

		g_assert_true(axis_db_migration_parse_filename(file, &version, &migration_name));
		g_assert_cmpuint(version, ==, i + 1);
		g_assert_true(g_file_get_contents(path, &sql, NULL, NULL));
		g_ptr_array_add(*owned, sql);
		g_ptr_array_add(*owned, migration_name);
		list[i].version = version;
		list[i].name = migration_name;
		list[i].sql = sql;
		list[i].no_transaction = axis_db_migration_has_no_transaction_marker(sql);
	}
	return list;
}

static void
assert_query(
	AxisTestDb  *db,
	const gchar *sql,
	const gchar *expected
){
	g_autofree gchar *got = axis_test_db_query(db, sql);

	if (g_strcmp0(got, expected) != 0)
		g_error("%s: expected %s, got %s", sql, expected != NULL ? expected : "NULL", got != NULL ? got : "NULL");
}

/* The imports of Usenet downloads recorded before this change were removed
 * from their clients believing their files went with them: each one is
 * looked at once more.  A torrent's, one not imported, and a manual import
 * are not touched. */
static void
test_migration_revisits_imported_usenet(void)
{
	g_autoptr(AxisTestDb) db = axis_test_db_new();
	g_autoptr(GPtrArray) owned = NULL;
	g_autoptr(GError) error = NULL;
	AxisDbMigration *list;
	AxisDbPool *pool;

	if (db == NULL)
		return;
	axis_test_log_init();
	pool = axis_db_pool_new(axis_test_db_get_url(db), 3, &error);
	g_assert_no_error(error);
	list = migrations_up_to(39, &owned);
	g_assert_true(axis_db_migrate_with(pool, list, 39, &error));
	g_assert_no_error(error);

	/* What the previous release recorded. */
	axis_test_db_exec(db,
		"INSERT INTO download (id, client_name, client_ref, protocol, source, release_name, state, volume_id, rel_path)"
		" OVERRIDING SYSTEM VALUE VALUES"
		" (1, 'Usenet', 'SABnzbd_nzo_made_up_1', 'usenet', 'file', 'Marrowfield.Lanterns.2011-MADEUP', 'imported',"
		"  NULL, 'downloads/Marrowfield.Lanterns.2011-MADEUP'),"
		" (2, 'Torrents', 'made-up-hash-2', 'torrent', 'magnet', 'Quillfeather.Junction.2014-MADEUP', 'imported', NULL,"
		"  'downloads/Quillfeather.Junction.2014-MADEUP'),"
		" (3, 'Usenet', 'SABnzbd_nzo_made_up_3', 'usenet', 'file', 'Bramblewick.Tides.2010-MADEUP', 'completed', NULL,"
		"  'downloads/Bramblewick.Tides.2010-MADEUP'),"
		" (4, 'Usenet', 'SABnzbd_nzo_made_up_4', 'usenet', 'file', 'Ostrander.Peaks.2018-MADEUP', 'imported', NULL,"
		"  'downloads/Ostrander.Peaks.2018-MADEUP');"
		"INSERT INTO import (id, module, title_label, download_id, source_path, protocol, state, finished_at,"
		" client_removal) OVERRIDING SYSTEM VALUE VALUES"
		" (11, 'movies', 'Marrowfield Lanterns (2011)', 1, 'downloads/Marrowfield.Lanterns.2011-MADEUP', 'usenet',"
		"  'imported', now(), 'done'),"
		" (12, 'movies', 'Quillfeather Junction (2014)', 2, 'downloads/Quillfeather.Junction.2014-MADEUP', 'torrent',"
		"  'imported', now(), 'done'),"
		" (13, 'movies', 'Bramblewick Tides (2010)', 3, 'downloads/Bramblewick.Tides.2010-MADEUP', 'usenet', 'waiting',"
		"  NULL, NULL),"
		" (14, 'movies', 'Ostrander Peaks (2018)', 4, 'downloads/Ostrander.Peaks.2018-MADEUP', 'usenet', 'imported',"
		"  now(), 'failed'),"
		" (15, 'movies', 'Bramblewick Tides (2010)', NULL, 'downloads/by-hand/Bramblewick.Tides.2010', NULL, 'imported',"
		"  now(), NULL);");

	g_assert_true(axis_db_migrate(pool, &error));
	g_assert_no_error(error);
	assert_query(db, "SELECT string_agg(id || '=' || coalesce(folder_removal, '-'), ',' ORDER BY id) FROM import",
		"11=pending,12=-,13=-,14=pending,15=-");
	assert_query(db, "SELECT count(*) FROM import WHERE folder_removal_text IS NOT NULL", "0");
	/* The states the column takes, and no other. */
	{
		g_autoptr(GError) failure = NULL;

		g_assert_false(axis_db_pool_exec(pool, "UPDATE import SET folder_removal = 'deleted' WHERE id = 11", 0, NULL,
			NULL, &failure));
		g_assert_nonnull(failure);
	}
	g_free(list);
	axis_db_pool_free(pool);
}

/* 0041: a client removal can be "gone" (the client no longer had it), and
 * nothing else new; the rows recorded before keep what they were, a failed
 * one to be asked again by the hourly cleanup. */
static void
test_migration_client_gone(void)
{
	g_autoptr(AxisTestDb) db = axis_test_db_new();
	g_autoptr(GPtrArray) owned = NULL;
	g_autoptr(GError) error = NULL;
	AxisDbMigration *list;
	AxisDbPool *pool;

	if (db == NULL)
		return;
	axis_test_log_init();
	pool = axis_db_pool_new(axis_test_db_get_url(db), 3, &error);
	g_assert_no_error(error);
	list = migrations_up_to(40, &owned);
	g_assert_true(axis_db_migrate_with(pool, list, 40, &error));
	g_assert_no_error(error);
	axis_test_db_exec(db,
		"INSERT INTO download (id, client_name, client_ref, protocol, source, release_name, state, volume_id, rel_path)"
		" OVERRIDING SYSTEM VALUE VALUES"
		" (1, 'Torrents', '0123456789abcdef0123456789abcdef01234567', 'torrent', 'magnet',"
		"  'Marrowfield.Lanterns.2011-MADEUP', 'imported', NULL, 'downloads/Marrowfield.Lanterns.2011-MADEUP');"
		"INSERT INTO import (id, module, title_label, download_id, source_path, protocol, state, finished_at,"
		" client_removal, client_removal_text) OVERRIDING SYSTEM VALUE VALUES"
		" (11, 'movies', 'Marrowfield Lanterns (2011)', 1, 'downloads/Marrowfield.Lanterns.2011-MADEUP', 'torrent',"
		"  'imported', now(), 'failed', 'Its client could not remove it yet (tried again every hour): Deluge refused it:"
		" Failure: [<class ''deluge.error.InvalidTorrentError''>: torrent_id 0123456789abcdef0123456789abcdef01234567"
		" not in session. ]');");
	/* Before 0041, "gone" is refused. */
	{
		g_autoptr(GError) failure = NULL;

		g_assert_false(axis_db_pool_exec(pool, "UPDATE import SET client_removal = 'gone' WHERE id = 11", 0, NULL, NULL,
			&failure));
		g_assert_nonnull(failure);
	}
	g_assert_true(axis_db_migrate(pool, &error));
	g_assert_no_error(error);
	/* The failed row is as it was, for the cleanup to ask again. */
	assert_query(db, "SELECT client_removal || ',' || coalesce(folder_removal, '-') FROM import WHERE id = 11",
		"failed,-");
	g_assert_true(axis_db_pool_exec(pool, "UPDATE import SET client_removal = 'gone' WHERE id = 11", 0, NULL, NULL,
		&error));
	g_assert_no_error(error);
	{
		g_autoptr(GError) failure = NULL;

		g_assert_false(axis_db_pool_exec(pool, "UPDATE import SET client_removal = 'vanished' WHERE id = 11", 0, NULL,
			NULL, &failure));
		g_assert_nonnull(failure);
	}
	/* A download's own leftover, and the inode of its place: nothing is set
	 * for the downloads recorded before. */
	assert_query(db, "SELECT coalesce(folder_removal, '-') || ',' || coalesce(location_inode::text, '-') FROM download"
		" WHERE id = 1", "-,-");
	g_assert_true(axis_db_pool_exec(pool, "UPDATE download SET folder_removal = 'pending', location_inode = 4242"
		" WHERE id = 1", 0, NULL, NULL, &error));
	g_assert_no_error(error);
	{
		g_autoptr(GError) failure = NULL;

		g_assert_false(axis_db_pool_exec(pool, "UPDATE download SET folder_removal = 'deleted' WHERE id = 1", 0, NULL,
			NULL, &failure));
		g_assert_nonnull(failure);
	}
	g_free(list);
	axis_db_pool_free(pool);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/import/folder/allowed", test_allowed);
	g_test_add_func("/import/folder/bad-paths", test_bad_paths);
	g_test_add_func("/import/folder/category", test_category);
	g_test_add_func("/import/folder/mapping", test_mapping);
	g_test_add_func("/import/folder/imported-from", test_imported_from);
	g_test_add_func("/import/folder/axis-own", test_axis_own);
	g_test_add_func("/import/folder/library-roots", test_library_roots);
	g_test_add_func("/import/folder/shared", test_shared);
	g_test_add_func("/import/folder/folder-reported", test_folder_reported);
	g_test_add_func("/import/folder/file-in-job-folder", test_file_in_the_job_folder);
	g_test_add_func("/import/folder/file-in-category", test_file_in_the_category_folder);
	g_test_add_func("/import/folder/file-rules", test_file_rules_are_the_job_folders);
	g_test_add_func("/import/folder/file-place", test_file_place_that_does_not_match);
	g_test_add_func("/import/folder/migration", test_migration_revisits_imported_usenet);
	g_test_add_func("/import/folder/job-folder", test_job_folder);
	g_test_add_func("/import/folder/source-is-job-folder", test_source_is_the_job_folder);
	g_test_add_func("/import/folder/torrent-label-folder", test_torrent_with_a_label_folder);
	g_test_add_func("/import/folder/torrent-no-own-folder", test_torrent_with_no_own_folder);
	g_test_add_func("/import/delete-download/rules", test_delete_download_rules);
	g_test_add_func("/import/folder/migration-client-gone", test_migration_client_gone);
	return g_test_run();
}
