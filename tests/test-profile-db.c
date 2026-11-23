/*
 * test-profile-db.c - conversion profiles in the database: the built-ins
 * written from the code table, immutable revisions, the lock, unusable
 * stored definitions, assignments, the estimates keyed by revision (a copy
 * estimates as its original, samples shared, the refresh policy, Maximum
 * compatibility's own threshold), and the migration of what the previous
 * schema held
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <glib.h>
#include <string.h>

#include "axis-test-analysis.h"
#include "axis-test-db.h"
#include "axis-test-library.h"
#include "axis-test-savings.h"
#include "db/axis-db-migrate.h"
#include "library/axis-analysis.h"
#include "transcode/axis-estimator.h"
#include "transcode/axis-profile-store.h"

#ifndef AXIS_MIGRATION_DIR
#error "AXIS_MIGRATION_DIR must be defined by the build"
#endif

typedef struct
{
	AxisTestLibrary *t;
	AxisSettings    *settings;
	AxisEstimator   *estimator;
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
	g_assert_true(axis_settings_load(fx->settings, NULL, &error));
	g_assert_no_error(error);
	fx->estimator = axis_estimator_new(fx->t->pool, fx->t->storage, fx->settings);
	if (data != NULL)
		axis_test_savings_build(fx->t, &fx->s);
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	if (fx->t == NULL) {
		axis_test_log_reset();
		return;
	}
	axis_estimator_unref(fx->estimator);
	axis_settings_unref(fx->settings);
	if (data != NULL)
		axis_test_savings_clear(&fx->s);
	axis_test_library_free(fx->t);
	axis_test_log_reset();
}

/* @sql is refused by the database. */
static void
refused(
	Fixture     *fx,
	const gchar *sql
){
	g_autoptr(GError) error = NULL;

	if (axis_db_pool_exec(fx->t->pool, sql, 0, NULL, NULL, &error))
		g_error("the database took: %s", sql);
}

static AxisProfileRevision *
current(
	Fixture     *fx,
	const gchar *slug
){
	g_autoptr(GError) error = NULL;
	AxisProfileRevision *rev = NULL;
	AxisDbConn *conn = axis_db_pool_acquire(fx->t->pool, &error);

	g_assert_nonnull(conn);
	g_assert_true(axis_profile_store_current(conn, slug, &rev, &error));
	axis_db_pool_release(fx->t->pool, conn);
	g_assert_no_error(error);
	return rev;
}

static AxisProfileDefinition
definition_of(const gchar *slug)
{
	AxisProfileDefinition def;

	g_assert_true(axis_profile_definition_parse(axis_profile_builtin(slug)->definition, -1, &def, NULL));
	return def;
}

static gchar *
copy_of(
	Fixture     *fx,
	const gchar *slug,
	const gchar *name
){
	g_autoptr(GError) error = NULL;
	gchar *created = NULL;

	g_assert_true(axis_profile_store_copy(fx->t->pool, slug, name, "root", &created, &error));
	g_assert_no_error(error);
	return created;
}

static void
save(
	Fixture                     *fx,
	const gchar                 *slug,
	gint                         expected,
	const AxisProfileDefinition *def,
	const gchar                 *name,
	AxisProfileSaved            *saved
){
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_profile_store_save(fx->t->pool, slug, expected, def, name, "", "root", saved, NULL, &error));
	g_assert_no_error(error);
}

static gint64
refresh(Fixture *fx)
{
	g_autoptr(GError) error = NULL;
	gint64 id = 0;

	g_assert_true(axis_estimator_refresh(fx->estimator, "direct", 0, NULL, &id, &error));
	g_assert_no_error(error);
	return id;
}

static AxisEstimateSnapshot *
snapshot(
	Fixture     *fx,
	const gchar *slug
){
	g_autoptr(GError) error = NULL;
	AxisEstimateSnapshot *snap = NULL;

	g_assert_true(axis_estimator_load_snapshot(fx->estimator, slug, &snap, &error));
	g_assert_no_error(error);
	return snap;
}

/* ── Built-ins ──────────────────────────────────────────────────── */

/* Spec: Built-ins present; a code change adds a revision; slugs reserved. */
static void
test_builtins(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	guint added = 99;
	guint i;

	SKIP_WITHOUT_DB(fx);
	/* A new database has them, at revision 1 of the code's own text. */
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM conversion_profile WHERE built_in"), ==,
		AXIS_PROFILE_N_BUILTINS);
	for (i = 0; i < AXIS_PROFILE_N_BUILTINS; i++) {
		const AxisProfileBuiltin *b = &axis_profile_builtins()[i];
		g_autoptr(AxisProfileRevision) rev = current(fx, b->slug);

		g_assert_nonnull(rev);
		g_assert_true(rev->usable);
		g_assert_true(rev->built_in);
		g_assert_cmpint(rev->number, ==, 1);
		g_assert_cmpstr(rev->text, ==, b->definition);
		g_assert_cmpstr(rev->origin, ==, "code");
		g_assert_false(axis_profile_revision_is_unlocked(rev));
	}
	/* Start-up writes nothing more when the code is what is stored. */
	g_assert_true(axis_profile_store_sync_builtins(fx->t->pool, &added, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(added, ==, 0);

	/* What another version's code had: the code of this one differs, so
	 * start-up adds a revision of it, and keeps the old ones. */
	axis_test_db_exec(fx->t->db,
		"INSERT INTO conversion_profile_revision (profile_id, number, name, definition, hash, origin)"
		" SELECT p.id, 2, 'Space saver', d.t, encode(sha256(convert_to(d.t, 'UTF8')), 'hex'), 'code'"
		" FROM conversion_profile p, (SELECT replace(definition, '\"min_saving_percent\":15', '\"min_saving_percent\":20')"
		"  AS t FROM conversion_profile_revision WHERE number = 1 AND profile_id = (SELECT id FROM conversion_profile"
		"  WHERE slug = 'space_saver')) d WHERE p.slug = 'space_saver';"
		"UPDATE conversion_profile SET current_revision_id = (SELECT id FROM conversion_profile_revision"
		" WHERE number = 2 AND profile_id = conversion_profile.id) WHERE slug = 'space_saver'");
	g_assert_true(axis_profile_store_sync_builtins(fx->t->pool, &added, &error));
	g_assert_cmpuint(added, ==, 1);
	{
		g_autoptr(AxisProfileRevision) rev = current(fx, "space_saver");

		g_assert_cmpint(rev->number, ==, 3);
		g_assert_cmpstr(rev->text, ==, axis_profile_builtin("space_saver")->definition);
		g_assert_false(axis_profile_revision_is_unlocked(rev));
	}
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM conversion_profile_revision r"
		" JOIN conversion_profile p ON p.id = r.profile_id WHERE p.slug = 'space_saver'"), ==, 3);

	/* A missing built-in is written. */
	axis_test_db_exec(fx->t->db, "ALTER TABLE conversion_profile DISABLE TRIGGER conversion_profile_guarded;"
		"UPDATE conversion_profile SET slug = 'gone_audiobooks' WHERE slug = 'audiobooks';"
		"ALTER TABLE conversion_profile ENABLE TRIGGER conversion_profile_guarded");
	g_assert_true(axis_profile_store_sync_builtins(fx->t->pool, &added, &error));
	g_assert_cmpuint(added, ==, 1);
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM conversion_profile WHERE slug = 'audiobooks'"
		" AND built_in"), ==, 1);

	/* Reserved: a user's profile is custom_<id>, never a built-in's slug. */
	refused(fx, "INSERT INTO conversion_profile (slug, kind, built_in) VALUES ('space_saver', 'video', false)");
	refused(fx, "INSERT INTO conversion_profile (slug, kind, built_in) VALUES ('my_saver', 'video', false)");
	refused(fx, "INSERT INTO conversion_profile (slug, kind, built_in) VALUES ('custom_9', 'video', true)");
}

/* Spec: Built-in cannot be edited. */
static void
test_builtin_read_only(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	AxisProfileDefinition def = definition_of("space_saver");
	AxisProfileSaved saved;

	SKIP_WITHOUT_DB(fx);
	def.min_saving_percent = 30;
	g_assert_false(axis_profile_store_save(fx->t->pool, "space_saver", 1, &def, "Mine", "", "root", &saved, NULL,
		&error));
	g_assert_error(error, AXIS_PROFILE_ERROR, AXIS_PROFILE_ERROR_BUILT_IN);
	g_clear_error(&error);
	g_assert_false(axis_profile_store_set_archived(fx->t->pool, "space_saver", TRUE, &error));
	g_assert_error(error, AXIS_PROFILE_ERROR, AXIS_PROFILE_ERROR_BUILT_IN);
	g_clear_error(&error);
	/* Nor around the code: the database takes no revision of a built-in
	 * from a user, and never archives one. */
	refused(fx, "INSERT INTO conversion_profile_revision (profile_id, number, name, definition, hash, origin,"
		" created_by) SELECT id, 2, 'x', '{}', encode(sha256(convert_to('{}', 'UTF8')), 'hex'), 'user', 'root'"
		" FROM conversion_profile WHERE slug = 'space_saver'");
	refused(fx, "UPDATE conversion_profile SET archived = true WHERE slug = 'space_saver'");
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM conversion_profile_revision"), ==,
		AXIS_PROFILE_N_BUILTINS);
}

/* ── Revisions ──────────────────────────────────────────────────── */

/* Spec: Edit makes a revision; No change; Archive.  Revisions are never
 * changed or deleted, by the store or by SQL. */
static void
test_revisions(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *slug = NULL;
	g_autofree gchar *first_text = NULL;
	AxisProfileDefinition def;
	AxisProfileSaved saved;

	SKIP_WITHOUT_DB(fx);
	slug = copy_of(fx, "space_saver", "Kids");
	g_assert_true(g_str_has_prefix(slug, "custom_"));
	{
		g_autoptr(AxisProfileRevision) rev = current(fx, slug);

		g_assert_cmpint(rev->number, ==, 1);
		g_assert_false(rev->built_in);
		g_assert_cmpstr(rev->text, ==, axis_profile_builtin("space_saver")->definition);
		first_text = g_strdup(rev->text);
	}

	/* No change: nothing added. */
	def = definition_of("space_saver");
	save(fx, slug, 1, &def, "Kids", &saved);
	g_assert_false(saved.created);
	g_assert_cmpint(saved.number, ==, 1);
	/* The same rules in another order mean the same: nothing added. */
	{
		AxisProfileRule r = def.rules[0];

		def.rules[0] = def.rules[1];
		def.rules[1] = r;
		save(fx, slug, 1, &def, "Kids", &saved);
		g_assert_false(saved.created);
	}

	/* An edit: a new revision, the earlier one unchanged. */
	def.crf = 24;
	save(fx, slug, 1, &def, "Kids", &saved);
	g_assert_true(saved.created);
	g_assert_cmpint(saved.number, ==, 2);
	{
		g_autofree gchar *now = axis_test_library_q(fx->t, "SELECT definition FROM conversion_profile_revision r JOIN"
			" conversion_profile p ON p.id = r.profile_id WHERE p.slug = '%s' AND r.number = 1", slug);

		g_assert_cmpstr(now, ==, first_text);
	}
	/* A rename alone is a revision too, with the same definition. */
	save(fx, slug, 2, &def, "Kids TV", &saved);
	g_assert_true(saved.created);
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(DISTINCT hash) FROM conversion_profile_revision r JOIN"
		" conversion_profile p ON p.id = r.profile_id WHERE p.slug = '%s' AND r.number IN (2, 3)", slug), ==, 1);
	/* An edit of an older revision than the current one is a conflict. */
	def.crf = 28;
	g_assert_false(axis_profile_store_save(fx->t->pool, slug, 1, &def, "Kids", "", "root", &saved, NULL, &error));
	g_assert_error(error, AXIS_PROFILE_ERROR, AXIS_PROFILE_ERROR_CONFLICT);
	g_clear_error(&error);
	/* A definition that does not check is refused, with its problems. */
	{
		g_autoptr(GPtrArray) problems = axis_profile_problems_new();

		def.min_saving_percent = 4;
		g_assert_false(axis_profile_store_save(fx->t->pool, slug, 3, &def, "Kids", "", "root", &saved, problems,
			&error));
		g_assert_error(error, AXIS_PROFILE_ERROR, AXIS_PROFILE_ERROR_INVALID);
		g_assert_nonnull(axis_profile_problems_find(problems, "min_saving_percent"));
		g_clear_error(&error);
	}

	/* Immutable in the database itself. */
	refused(fx, "UPDATE conversion_profile_revision SET note = 'changed'");
	refused(fx, "UPDATE conversion_profile_revision SET definition = definition");
	refused(fx, "DELETE FROM conversion_profile_revision WHERE number = 1");
	refused(fx, "TRUNCATE conversion_profile_revision CASCADE");
	refused(fx, "DELETE FROM conversion_profile");
	refused(fx, "UPDATE conversion_profile SET kind = 'audio'");
	refused(fx, "UPDATE conversion_profile SET slug = 'custom_999' WHERE NOT built_in");
	/* The current revision only moves on. */
	refused(fx, "UPDATE conversion_profile SET current_revision_id = (SELECT id FROM conversion_profile_revision r"
		" WHERE r.profile_id = conversion_profile.id AND r.number = 1) WHERE NOT built_in");
	/* A revision out of order is refused. */
	refused(fx, "INSERT INTO conversion_profile_revision (profile_id, number, name, definition, hash, origin,"
		" created_by) SELECT p.id, 9, 'x', r.definition, r.hash, 'user', 'root' FROM conversion_profile p"
		" JOIN conversion_profile_revision r ON r.id = p.current_revision_id WHERE NOT p.built_in");
	/* A hash that is not the definition's is refused. */
	refused(fx, "INSERT INTO conversion_profile_revision (profile_id, number, name, definition, hash, origin,"
		" created_by) SELECT p.id, 4, 'x', r.definition, 'deadbeef', 'user', 'root' FROM conversion_profile p"
		" JOIN conversion_profile_revision r ON r.id = p.current_revision_id WHERE NOT p.built_in");

	/* Whatever refers to a revision holds it (and would were the trigger
	 * gone): every reference is ON DELETE RESTRICT. */
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM pg_constraint c"
		" WHERE c.contype = 'f' AND c.confrelid = 'conversion_profile_revision'::regclass"
		" AND c.confdeltype <> 'r'"), ==, 0);
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(DISTINCT conrelid) FROM pg_constraint c"
		" WHERE c.contype = 'f' AND c.confrelid = 'conversion_profile_revision'::regclass"
		" AND conrelid::regclass::text IN ('estimate_fact', 'estimate_sample', 'conversion_profile_assignment',"
		" 'conversion_profile_proof', 'estimate_refresh_request', 'conversion_profile')"), ==, 6);
	axis_test_db_exec(fx->t->db, "ALTER TABLE conversion_profile_revision DISABLE TRIGGER conversion_profile_revision_no_change");
	refused(fx, "DELETE FROM conversion_profile_revision WHERE id IN (SELECT current_revision_id FROM conversion_profile)");
	axis_test_db_exec(fx->t->db, "ALTER TABLE conversion_profile_revision ENABLE TRIGGER conversion_profile_revision_no_change");

	/* Archive: not offered, record kept, and back again. */
	g_assert_true(axis_profile_store_set_archived(fx->t->pool, slug, TRUE, &error));
	g_assert_no_error(error);
	{
		g_autoptr(GPtrArray) shown = NULL;
		g_autoptr(GPtrArray) all = NULL;
		AxisDbConn *conn = axis_db_pool_acquire(fx->t->pool, &error);

		g_assert_true(axis_profile_store_list(conn, FALSE, &shown, &error));
		g_assert_true(axis_profile_store_list(conn, TRUE, &all, &error));
		axis_db_pool_release(fx->t->pool, conn);
		g_assert_cmpuint(shown->len, ==, AXIS_PROFILE_N_BUILTINS);
		g_assert_cmpuint(all->len, ==, AXIS_PROFILE_N_BUILTINS + 1);
	}
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM conversion_profile_revision r JOIN"
		" conversion_profile p ON p.id = r.profile_id WHERE p.slug = '%s'", slug), ==, 3);
	g_assert_true(axis_profile_store_set_archived(fx->t->pool, slug, FALSE, &error));
	g_assert_no_error(error);
}

/* ── The lock ───────────────────────────────────────────────────── */

/* Spec: New profiles start locked; Output-affecting change locks; Other
 * change keeps the proof.  A proof is inserted by hand here, as the canary
 * change will: nothing in this change makes one. */
static void
test_lock(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *slug = NULL;
	AxisProfileDefinition def = definition_of("space_saver");
	AxisProfileSaved saved;

	SKIP_WITHOUT_DB(fx);
	slug = copy_of(fx, "space_saver", "Proven");
	{
		g_autoptr(AxisProfileRevision) rev = current(fx, slug);

		g_assert_false(axis_profile_revision_is_unlocked(rev));
		/* Without a proof, an edit that keeps the bytes keeps it locked. */
		def.min_saving_percent = 25;
		save(fx, slug, 1, &def, "Proven", &saved);
		g_assert_false(saved.carried);
	}
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM conversion_profile_proof"), ==, 0);

	/* As if its canary set had been confirmed. */
	axis_test_db_exec(fx->t->db, "INSERT INTO conversion_profile_proof (revision_id, method, recorded_by)"
		" SELECT current_revision_id, 'canary', 'test' FROM conversion_profile WHERE NOT built_in");
	{
		g_autoptr(AxisProfileRevision) rev = current(fx, slug);

		g_assert_true(axis_profile_revision_is_unlocked(rev));
	}
	/* The name or the minimum saving: the proof carries. */
	def.min_saving_percent = 30;
	save(fx, slug, 2, &def, "Proven again", &saved);
	g_assert_true(saved.carried);
	{
		g_autoptr(AxisProfileRevision) rev = current(fx, slug);

		g_assert_true(axis_profile_revision_is_unlocked(rev));
		g_assert_cmpstr(rev->proof_method, ==, "carried");
	}
	/* The quality rule: locked. */
	def.crf = 22;
	save(fx, slug, 3, &def, "Proven again", &saved);
	g_assert_false(saved.carried);
	{
		g_autoptr(AxisProfileRevision) rev = current(fx, slug);

		g_assert_false(axis_profile_revision_is_unlocked(rev));
	}
	/* Withdrawing the original proof withdraws what was carried from it. */
	axis_test_db_exec(fx->t->db, "DELETE FROM conversion_profile_proof WHERE method = 'canary'");
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM conversion_profile_proof"), ==, 0);
	/* A proof is never changed, and is only carried within a profile and
	 * forward. */
	axis_test_db_exec(fx->t->db, "INSERT INTO conversion_profile_proof (revision_id, method, recorded_by)"
		" SELECT id, 'canary', 'test' FROM conversion_profile_revision WHERE number = 1 AND profile_id ="
		" (SELECT id FROM conversion_profile WHERE NOT built_in)");
	refused(fx, "UPDATE conversion_profile_proof SET method = 'carried'");
	refused(fx, "INSERT INTO conversion_profile_proof (revision_id, method, carried_from) SELECT r.id, 'carried',"
		" (SELECT revision_id FROM conversion_profile_proof LIMIT 1) FROM conversion_profile_revision r"
		" JOIN conversion_profile p ON p.id = r.profile_id WHERE p.slug = 'balanced'");
	/* The built-ins are all locked: nothing has proven them. */
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM conversion_profile p JOIN conversion_profile_proof"
		" pr ON pr.revision_id = p.current_revision_id WHERE p.built_in"), ==, 0);
}

/* ── A stored definition that does not validate ─────────────────── */

/* Spec: Stored definition no longer valid.  Unusable everywhere, and said
 * why; nothing repairs it. */
static void
test_unusable(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *slug = NULL;
	g_autofree gchar *other = NULL;
	AxisEstimateTarget target;

	SKIP_WITHOUT_DB(fx);
	slug = copy_of(fx, "space_saver", "Old");
	other = copy_of(fx, "space_saver", "Spaced");
	/* Assigned while it was good: it is refreshed on schedule. */
	g_assert_true(axis_profile_store_assign(fx->t->pool, fx->s.films_a, other, "unknown", TRUE, "root", &error));
	g_assert_no_error(error);
	/* An older format, as an upgrade could leave behind; and the same
	 * definition written with spaces (not this version's form). */
	{
		g_autofree gchar *old_sql = g_strdup_printf(
			"INSERT INTO conversion_profile_revision (profile_id, number, name, definition, hash, origin, created_by)"
			" SELECT id, 2, 'Old', d, encode(sha256(convert_to(d, 'UTF8')), 'hex'), 'user', 'root' FROM"
			" (SELECT p.id, replace(r.definition, '\"format\":1', '\"format\":0') AS d FROM conversion_profile p"
			"  JOIN conversion_profile_revision r ON r.id = p.current_revision_id WHERE p.slug = '%s') x;"
			"UPDATE conversion_profile SET current_revision_id = (SELECT id FROM conversion_profile_revision r"
			" WHERE r.profile_id = conversion_profile.id AND r.number = 2) WHERE slug = '%s'", slug, slug);
		g_autofree gchar *spaced_sql = g_strdup_printf(
			"INSERT INTO conversion_profile_revision (profile_id, number, name, definition, hash, origin, created_by)"
			" SELECT id, 2, 'Spaced', d, encode(sha256(convert_to(d, 'UTF8')), 'hex'), 'user', 'root' FROM"
			" (SELECT p.id, replace(r.definition, ',', ', ') AS d FROM conversion_profile p"
			"  JOIN conversion_profile_revision r ON r.id = p.current_revision_id WHERE p.slug = '%s') x;"
			"UPDATE conversion_profile SET current_revision_id = (SELECT id FROM conversion_profile_revision r"
			" WHERE r.profile_id = conversion_profile.id AND r.number = 2) WHERE slug = '%s'", other, other);

		axis_test_db_exec(fx->t->db, old_sql);
		axis_test_db_exec(fx->t->db, spaced_sql);
	}
	{
		g_autoptr(AxisProfileRevision) rev = current(fx, slug);
		g_autoptr(AxisProfileRevision) spaced = current(fx, other);

		g_assert_false(rev->usable);
		g_assert_nonnull(strstr(rev->unusable, "does not pass the checks of this Axis release"));
		g_assert_false(spaced->usable);
		g_assert_nonnull(strstr(spaced->unusable, "not in the form this version writes"));
		g_assert_false(axis_profile_revision_is_unlocked(rev));
		g_assert_false(axis_profile_revision_target(rev, &target));
	}
	/* No estimate, no copy, no edit, no assignment. */
	g_assert_false(axis_estimator_profile_target(fx->estimator, slug, 0, &target, NULL, &error));
	g_assert_error(error, AXIS_ESTIMATE_ERROR, AXIS_ESTIMATE_ERROR_FAILED);
	g_clear_error(&error);
	{
		g_autofree gchar *none = NULL;

		g_assert_false(axis_profile_store_copy(fx->t->pool, slug, "Again", "root", &none, &error));
		g_assert_error(error, AXIS_PROFILE_ERROR, AXIS_PROFILE_ERROR_UNUSABLE);
		g_assert_null(none);
		g_clear_error(&error);
	}
	{
		AxisProfileDefinition def = definition_of("space_saver");
		AxisProfileSaved saved;

		g_assert_false(axis_profile_store_save(fx->t->pool, slug, 2, &def, "Fixed", "", "root", &saved, NULL, &error));
		g_assert_error(error, AXIS_PROFILE_ERROR, AXIS_PROFILE_ERROR_UNUSABLE);
		g_clear_error(&error);
	}
	g_assert_false(axis_profile_store_assign(fx->t->pool, fx->s.films_a, slug, "safe", FALSE, "root", &error));
	g_assert_error(error, AXIS_PROFILE_ERROR, AXIS_PROFILE_ERROR_UNUSABLE);
	g_clear_error(&error);
	/* Its earlier, valid revision is still there and still valid. */
	{
		g_autoptr(AxisProfileRevision) first = NULL;
		AxisDbConn *conn = axis_db_pool_acquire(fx->t->pool, &error);

		g_assert_true(axis_profile_store_revision(conn, slug, 1, &first, &error));
		axis_db_pool_release(fx->t->pool, conn);
		g_assert_true(first->usable);
	}
	/* Nor is it estimated in a refresh: asked for, the request is refused;
	 * assigned, it is left out (and said so). */
	g_assert_false(axis_estimator_request_profile_refresh(fx->estimator, slug, "root", NULL, &error));
	g_clear_error(&error);
	refresh(fx);
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM estimate_fact f JOIN conversion_profile_revision r"
		" ON r.id = f.revision_id JOIN conversion_profile p ON p.id = r.profile_id WHERE p.slug IN ('%s', '%s')", slug,
		other), ==, 0);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*revision 2 is not estimated*"), >=, 1);
	/* The file page shows the built-ins only. */
	{
		g_autoptr(GArray) estimates = NULL;
		gint64 id = axis_test_scenario_file(fx->t, fx->s.films_a, "Big (2001)/Big (2001).mkv");

		g_assert_true(axis_estimator_estimate_file(fx->estimator, id, &estimates, &error));
		g_assert_cmpuint(estimates->len, ==, 3);
	}
}

/* ── Estimates by revision ──────────────────────────────────────── */

static void
assert_same_facts(
	const AxisEstimateSnapshot *snap,
	const gchar                *a,
	const gchar                *b
){
	g_autoptr(GPtrArray) x = g_ptr_array_new();
	g_autoptr(GPtrArray) y = g_ptr_array_new();
	guint i;

	for (i = 0; i < snap->facts->len; i++) {
		AxisEstimateFact *f = g_ptr_array_index(snap->facts, i);

		if (strcmp(f->target, a) == 0)
			g_ptr_array_add(x, f);
		else if (strcmp(f->target, b) == 0)
			g_ptr_array_add(y, f);
	}
	g_assert_cmpuint(x->len, >, 5);
	g_assert_cmpuint(x->len, ==, y->len);
	for (i = 0; i < x->len; i++) {
		const AxisEstimateFact *f = g_ptr_array_index(x, i);
		const AxisEstimateFact *g = g_ptr_array_index(y, i);

		g_assert_cmpstr(f->report, ==, g->report);
		g_assert_cmpstr(f->dim, ==, g->dim);
		g_assert_cmpint(f->paths, ==, g->paths);
		g_assert_cmpint(f->bytes, ==, g->bytes);
		g_assert_cmpint(f->low, ==, g->low);
		g_assert_cmpint(f->mid, ==, g->mid);
		g_assert_cmpint(f->high, ==, g->high);
	}
}

/* Spec: Copy (its estimates equal the original's); Custom profile (its
 * figures, and the revision named); refresh policy: built-in and assigned
 * on schedule, others on request. */
static void
test_estimates(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *assigned = NULL;
	g_autofree gchar *asked = NULL;
	g_autofree gchar *idle = NULL;

	SKIP_WITHOUT_DB(fx);
	assigned = copy_of(fx, "space_saver", "Assigned copy");
	asked = copy_of(fx, "music_space_saver", "Asked copy");
	idle = copy_of(fx, "balanced", "Idle copy");
	g_assert_true(axis_profile_store_assign(fx->t->pool, fx->s.films_a, assigned, "unknown", TRUE, "root", &error));
	g_assert_no_error(error);
	g_assert_true(axis_estimator_request_profile_refresh(fx->estimator, asked, "root", NULL, &error));
	g_assert_no_error(error);
	refresh(fx);
	{
		g_autoptr(AxisEstimateSnapshot) snap = snapshot(fx, NULL);
		const AxisEstimateFact *copy_total = axis_estimate_snapshot_fact(snap, assigned, "total", "");
		const AxisEstimateFact *total = axis_estimate_snapshot_fact(snap, "space_saver", "total", "");

		/* Built-ins, the assigned and the asked-for profile; not the idle. */
		g_assert_nonnull(axis_estimate_snapshot_fact(snap, "max_compat", "total", ""));
		g_assert_nonnull(copy_total);
		g_assert_nonnull(axis_estimate_snapshot_fact(snap, asked, "total", ""));
		g_assert_null(axis_estimate_snapshot_fact(snap, idle, "total", ""));
		/* A copy, unedited, estimates exactly as its original. */
		g_assert_cmpint(copy_total->paths, ==, total->paths);
		g_assert_cmpint(copy_total->mid, ==, total->mid);
		assert_same_facts(snap, "space_saver", assigned);
		assert_same_facts(snap, "music_space_saver", asked);
		/* Each figure names its revision. */
		g_assert_cmpint(copy_total->revision, ==, 1);
		g_assert_cmpint(copy_total->revision_id, ==, axis_test_library_n(fx->t, "SELECT current_revision_id FROM"
			" conversion_profile WHERE slug = '%s'", assigned));
	}
	/* The request was answered; the next scheduled refresh leaves the
	 * asked-for profile out, and its figures stay in the older snapshot. */
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM estimate_refresh_request"), ==, 0);
	refresh(fx);
	{
		g_autoptr(AxisEstimateSnapshot) latest = snapshot(fx, NULL);
		g_autoptr(AxisEstimateSnapshot) its = snapshot(fx, asked);

		g_assert_null(axis_estimate_snapshot_fact(latest, asked, "total", ""));
		g_assert_nonnull(axis_estimate_snapshot_fact(latest, assigned, "total", ""));
		g_assert_nonnull(its);
		g_assert_cmpint(its->id, <, latest->id);
		g_assert_nonnull(axis_estimate_snapshot_fact(its, asked, "total", ""));
	}
	/* Lists keyed by profile and revision agree with the figures. */
	{
		g_autoptr(AxisEstimateSnapshot) snap = snapshot(fx, NULL);
		const AxisEstimateFact *total = axis_estimate_snapshot_fact(snap, assigned, "total", "");
		g_autoptr(AxisFileList) list = NULL;
		AxisLibraryFilter filter;
		g_autofree gchar *query = g_strdup_printf("target=%s&rev=1&outcome=eligible", assigned);

		g_assert_true(axis_library_filter_parse(query, &filter, &error));
		g_assert_true(axis_estimator_list_files(fx->estimator, &filter, &list, &error));
		g_assert_no_error(error);
		g_assert_cmpint(list->count, ==, total->paths);
		axis_library_filter_clear(&filter);
		/* A revision that is not there, a profile that is not there. */
		g_clear_pointer(&list, axis_file_list_free);
		g_free(query);
		query = g_strdup_printf("target=%s&rev=7", assigned);
		g_assert_true(axis_library_filter_parse(query, &filter, &error));
		g_assert_false(axis_estimator_list_files(fx->estimator, &filter, &list, &error));
		g_assert_error(error, AXIS_ESTIMATE_ERROR, AXIS_ESTIMATE_ERROR_NOT_FOUND);
		g_clear_error(&error);
		axis_library_filter_clear(&filter);
	}
}

/* Spec: Samples shared where the output is the same.  A sample recorded for
 * a copy's revision calibrates the built-in it was copied from (and any
 * revision encoding to the same cell). */
static void
test_samples_shared(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *slug = NULL;
	AxisEstimateSample sample;
	AxisEstimateModel *model;
	guint cells[16];
	guint n;
	guint i;
	guint samples = 0;

	SKIP_WITHOUT_DB(fx);
	slug = copy_of(fx, "space_saver", "Measured copy");
	memset(&sample, 0, sizeof(sample));
	sample.target = slug;
	sample.revision_id = axis_test_library_n(fx->t, "SELECT current_revision_id FROM conversion_profile WHERE slug"
		" = '%s'", slug);
	sample.has_output_facts = TRUE;
	sample.out_bit_depth = 10;
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
	sample.output_bitrate = 1500000;
	g_assert_true(axis_estimator_record_sample(fx->estimator, NULL, &sample, "measured", 0, 0, &error));
	g_assert_no_error(error);
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM estimate_sample WHERE revision_id = %"
		G_GINT64_FORMAT, sample.revision_id), ==, 1);
	model = axis_estimator_get_model(fx->estimator, &error);
	g_assert_nonnull(model);
	n = axis_estimate_model_target_cells(model, axis_estimate_builtin_target("space_saver"), cells, G_N_ELEMENTS(cells));
	for (i = 0; i < n; i++) {
		AxisEstimateCellInfo info;

		axis_estimate_model_cell_info(model, cells[i], &info);
		samples += info.samples;
	}
	g_assert_cmpuint(samples, ==, 1);
	g_assert_cmpint(axis_estimate_model_state_of(model, cells, n), ==, AXIS_CALIBRATION_PARTLY);
	axis_estimator_model_unref(model);
}

/* Spec (4.10): Maximum compatibility now converts large H.264 of the
 * fixture library; with the Analysis outlier threshold it used before (a
 * copy set to 0.300) it converts none of them. */
static void
test_max_compat_threshold(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *before = NULL;
	AxisProfileDefinition def = definition_of("max_compat");
	AxisProfileSaved saved;

	SKIP_WITHOUT_DB(fx);
	before = copy_of(fx, "max_compat", "As before");
	def.efficient_milli = 300;
	save(fx, before, 1, &def, "As before", &saved);
	g_assert_true(axis_estimator_request_profile_refresh(fx->estimator, before, "root", NULL, &error));
	refresh(fx);
	{
		g_autoptr(AxisEstimateSnapshot) snap = snapshot(fx, NULL);
		const AxisEstimateFact *now = axis_estimate_snapshot_fact(snap, "max_compat", "total", "");
		const AxisEstimateFact *then = axis_estimate_snapshot_fact(snap, before, "total", "");
		const AxisEstimateFact *efficient_now = axis_estimate_snapshot_fact(snap, "max_compat", "skip",
			"already_efficient");
		const AxisEstimateFact *efficient_then = axis_estimate_snapshot_fact(snap, before, "skip", "already_efficient");

		g_assert_nonnull(now);
		g_assert_nonnull(then);
		g_test_message("Maximum compatibility on the fixture library: before (0.300) %" G_GINT64_FORMAT " files, %"
			G_GINT64_FORMAT " bytes now, saving %" G_GINT64_FORMAT " (middle), %" G_GINT64_FORMAT " already efficient;"
			" after (0.120) %" G_GINT64_FORMAT " files, %" G_GINT64_FORMAT " bytes now, saving %" G_GINT64_FORMAT
			", %" G_GINT64_FORMAT " already efficient", then->paths, then->bytes, then->bytes - then->mid,
			efficient_then != NULL ? efficient_then->paths : 0, now->paths, now->bytes, now->bytes - now->mid,
			efficient_now != NULL ? efficient_now->paths : 0);
		g_assert_cmpint(then->paths, ==, 0);
		g_assert_cmpint(now->paths, >=, 3);
		g_assert_cmpint(now->bytes - now->mid, >, 0);
	}
	/* Big, Other and the progressive episode: H.264 at 0.16 to 0.20. */
	{
		g_autofree gchar *query = g_strdup("target=max_compat&outcome=eligible");
		g_autoptr(AxisFileList) list = NULL;
		AxisLibraryFilter filter;

		g_assert_true(axis_library_filter_parse(query, &filter, &error));
		g_assert_true(axis_estimator_list_files(fx->estimator, &filter, &list, &error));
		g_assert_no_error(error);
		g_assert_cmpint(list->count, >=, 3);
		axis_library_filter_clear(&filter);
	}
}

/* The edit page's preview counts with both definitions. */
static void
test_preview(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	AxisProfileDefinition def = definition_of("space_saver");
	AxisEstimateTarget before;
	AxisEstimateTarget built;
	AxisEstimateTarget after;
	AxisEstimatePreview preview;

	SKIP_WITHOUT_DB(fx);
	g_assert_true(axis_estimator_profile_target(fx->estimator, "space_saver", 0, &before, NULL, &error));
	def.min_saving_percent = 90;
	g_assert_true(axis_profile_to_target(&def, NULL, &built));
	axis_estimator_configure(fx->estimator, &built, &after);
	g_assert_true(axis_estimator_preview(fx->estimator, &before, &after, 10000, &preview, &error));
	g_assert_no_error(error);
	g_assert_true(preview.complete);
	g_assert_cmpint(preview.read, ==, preview.total);
	g_assert_cmpint(preview.before.scope, ==, preview.after.scope);
	g_assert_cmpint(preview.before.converts, >, 0);
	g_assert_cmpint(preview.after.converts, <, preview.before.converts);
	g_assert_cmpint(preview.after.skips[AXIS_ESTIMATE_WOULD_NOT_SHRINK], >,
		preview.before.skips[AXIS_ESTIMATE_WOULD_NOT_SHRINK]);
	/* Nothing stored. */
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM estimate_snapshot"), ==, 0);
}

/* ── Assignment ─────────────────────────────────────────────────── */

/* Spec: Assign (no file changed, no job); Risky or unknown (a second
 * confirmation); one video and one audio profile per root. */
static void
test_assignment(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *index_before = NULL;
	g_autofree gchar *index_after = NULL;
	g_autofree gchar *copy = NULL;
	g_autoptr(GPtrArray) assignments = NULL;
	gint64 jobs;
	AxisDbConn *conn;

	SKIP_WITHOUT_DB(fx);
	index_before = axis_test_library_index(fx->t, fx->s.films_a);
	jobs = axis_test_library_n(fx->t, "SELECT count(*) FROM job");
	copy = copy_of(fx, "balanced", "Family");

	/* Unknown or risky needs the confirmation; safe does not. */
	g_assert_false(axis_profile_store_assign(fx->t->pool, fx->s.films_a, "space_saver", "unknown", FALSE, "root",
		&error));
	g_assert_error(error, AXIS_PROFILE_ERROR, AXIS_PROFILE_ERROR_CONFIRM);
	g_clear_error(&error);
	g_assert_false(axis_profile_store_assign(fx->t->pool, fx->s.films_a, "space_saver", "risky", FALSE, "root",
		&error));
	g_assert_error(error, AXIS_PROFILE_ERROR, AXIS_PROFILE_ERROR_CONFIRM);
	g_clear_error(&error);
	refused(fx, "INSERT INTO conversion_profile_assignment (root_id, kind, profile_id, revision_id, verdict,"
		" confirmed, assigned_by) SELECT r.id, 'video', p.id, p.current_revision_id, 'risky', false, 'root'"
		" FROM library_root r, conversion_profile p WHERE r.name = 'Films' AND p.slug = 'space_saver'");
	g_assert_true(axis_profile_store_assign(fx->t->pool, fx->s.films_a, "space_saver", "risky", TRUE, "root", &error));
	g_assert_no_error(error);
	/* A second video profile replaces the first; an audio one sits beside
	 * it only on a root it is for. */
	g_assert_true(axis_profile_store_assign(fx->t->pool, fx->s.films_a, copy, "safe", FALSE, "root", &error));
	g_assert_no_error(error);
	g_assert_false(axis_profile_store_assign(fx->t->pool, fx->s.films_a, "music_compat", "safe", FALSE, "root",
		&error));
	g_assert_error(error, AXIS_PROFILE_ERROR, AXIS_PROFILE_ERROR_MISMATCH);
	g_clear_error(&error);
	g_assert_true(axis_profile_store_assign(fx->t->pool, fx->s.music_a, "music_compat", "safe", FALSE, "root", &error));
	g_assert_true(axis_profile_store_assign(fx->t->pool, fx->s.music_a, "keep_lossless", "safe", FALSE, "root",
		&error));
	g_assert_false(axis_profile_store_assign(fx->t->pool, fx->s.down_a, "space_saver", "safe", FALSE, "root", &error));
	g_assert_error(error, AXIS_PROFILE_ERROR, AXIS_PROFILE_ERROR_MISMATCH);
	g_clear_error(&error);
	g_assert_false(axis_profile_store_assign(fx->t->pool, 999999, "space_saver", "safe", FALSE, "root", &error));
	g_assert_error(error, AXIS_PROFILE_ERROR, AXIS_PROFILE_ERROR_NOT_FOUND);
	g_clear_error(&error);
	conn = axis_db_pool_acquire(fx->t->pool, &error);
	g_assert_true(axis_profile_store_assignments(conn, &assignments, &error));
	axis_db_pool_release(fx->t->pool, conn);
	g_assert_cmpuint(assignments->len, ==, 2);
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM conversion_profile_assignment WHERE root_id = %"
		G_GINT64_FORMAT, fx->s.films_a), ==, 1);
	{
		g_autofree gchar *music = axis_test_library_q(fx->t, "SELECT p.slug FROM conversion_profile_assignment a JOIN"
			" conversion_profile p ON p.id = a.profile_id WHERE a.root_id = %" G_GINT64_FORMAT, fx->s.music_a);

		g_assert_cmpstr(music, ==, "keep_lossless");
	}
	/* An assigned profile is not archived. */
	g_assert_false(axis_profile_store_set_archived(fx->t->pool, copy, TRUE, &error));
	g_assert_error(error, AXIS_PROFILE_ERROR, AXIS_PROFILE_ERROR_ASSIGNED);
	g_clear_error(&error);
	/* An archived one is not assigned. */
	{
		g_autofree gchar *spare = copy_of(fx, "balanced", "Spare");

		g_assert_true(axis_profile_store_set_archived(fx->t->pool, spare, TRUE, &error));
		g_assert_false(axis_profile_store_assign(fx->t->pool, fx->s.shows_a, spare, "safe", FALSE, "root", &error));
		g_assert_error(error, AXIS_PROFILE_ERROR, AXIS_PROFILE_ERROR_ARCHIVED);
		g_clear_error(&error);
	}

	/* Nothing but the intent: no job, no file changed. */
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM job"), ==, jobs);
	index_after = axis_test_library_index(fx->t, fx->s.films_a);
	g_assert_cmpstr(index_before, ==, index_after);
	{
		gboolean removed = FALSE;

		g_assert_true(axis_profile_store_unassign(fx->t->pool, fx->s.films_a, AXIS_PROFILE_KIND_VIDEO, &removed,
			&error));
		g_assert_true(removed);
	}
}

/* ── The previous schema ────────────────────────────────────────── */

static gint
compare_names(
	gconstpointer a,
	gconstpointer b
){
	return strcmp(*(const gchar *const *)a, *(const gchar *const *)b);
}

static AxisDbMigration *
migrations_up_to(
	guint        last,
	GPtrArray  **owned
){
	g_autoptr(GDir) dir = g_dir_open(AXIS_MIGRATION_DIR, 0, NULL);
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
		g_autofree gchar *path = g_build_filename(AXIS_MIGRATION_DIR, file, NULL);
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

/* What the previous schema held (snapshots and samples keyed by target
 * id) is carried over to the built-ins' first revisions: nothing dropped,
 * every fact and sample of a target on that target's profile, and the
 * samples still calibrate. */
static void
test_migration(void)
{
	g_autoptr(AxisTestDb) db = axis_test_db_new();
	g_autoptr(GPtrArray) owned = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *facts_before = NULL;
	g_autofree gchar *facts_after = NULL;
	g_autofree gchar *samples_before = NULL;
	g_autofree gchar *samples_after = NULL;
	AxisDbPool *pool;
	AxisDbMigration *list;
	g_autofree gchar *count = NULL;

	if (db == NULL)
		return;
	axis_test_log_init();
	pool = axis_db_pool_new(axis_test_db_get_url(db), 3, &error);
	g_assert_no_error(error);
	list = migrations_up_to(13, &owned);
	g_assert_true(axis_db_migrate_with(pool, list, 13, &error));
	g_assert_no_error(error);
	g_free(list);

	/* What the previous release wrote. */
	axis_test_db_exec(db,
		"INSERT INTO setting (key, value) VALUES ('estimate.min_saving_video', '0.25'),"
		" ('estimate.encoder', '\"nvenc\"');"
		"INSERT INTO estimate_snapshot (taken_at, trigger, took_ms, coverage, config) VALUES"
		" (now() - interval '1 hour', 'schedule', 5, '{}', '{\"targets\": {\"space_saver\": {\"name\": \"Space saver\"}}}');"
		"INSERT INTO estimate_fact (snapshot_id, target, report, dim, paths, bytes, low, mid, high)"
		" SELECT 1, t, 'total', '', 3, 3000, 1000, 1500, 2000 FROM unnest(ARRAY['space_saver', 'balanced',"
		" 'max_compat', 'keep_lossless', 'music_space_saver', 'music_compat', 'audiobooks']) AS t;"
		"INSERT INTO estimate_fact (snapshot_id, target, report, dim, paths, bytes) VALUES"
		" (1, 'space_saver', 'skip', 'pinned', 2, 900);"
		"INSERT INTO estimate_sample (origin, target, codec, family, quality, part, source_codec, width, height,"
		" frame_rate, source_bitrate, seconds, output_bitrate) VALUES"
		" ('measured', 'space_saver', 'hevc', 'software', 4, 'video', 'h264', 1920, 1080, 24, 8000000, 60, 1500000),"
		" ('measured', 'balanced', 'hevc', 'software', 4, 'video', 'h264', 1920, 1080, 24, 8000000, 60, 1600000);"
		"INSERT INTO estimate_sample (origin, target, codec, family, quality, part, source_codec, source_bitrate,"
		" sample_rate, channels, bit_depth, seconds, output_bitrate) VALUES"
		" ('measured', 'audiobooks', 'opus', 'software', 4, 'audio', 'mp3', 128000, 44100, 1, 16, 60, 33000),"
		" ('measured', 'music_space_saver', 'opus', 'software', 4, 'audio', 'flac', 900000, 44100, 2, 16, 60, 130000)");
	facts_before = axis_test_db_query(db, "SELECT string_agg(target || '/' || report || '/' || dim || '/' || paths"
		" || '/' || mid, ',' ORDER BY target, report, dim) FROM estimate_fact");
	samples_before = axis_test_db_query(db, "SELECT string_agg(target || '/' || codec || '/' || output_bitrate, ','"
		" ORDER BY id) FROM estimate_sample");

	/* The current schema. */
	g_assert_true(axis_db_migrate(pool, &error));
	g_assert_no_error(error);
	facts_after = axis_test_db_query(db, "SELECT string_agg(p.slug || '/' || f.report || '/' || f.dim || '/' ||"
		" f.paths || '/' || f.mid, ',' ORDER BY p.slug, f.report, f.dim) FROM estimate_fact f"
		" JOIN conversion_profile_revision r ON r.id = f.revision_id JOIN conversion_profile p ON p.id = r.profile_id"
		" WHERE r.number = 1 AND p.built_in");
	samples_after = axis_test_db_query(db, "SELECT string_agg(p.slug || '/' || s.codec || '/' || s.output_bitrate, ','"
		" ORDER BY s.id) FROM estimate_sample s JOIN conversion_profile_revision r ON r.id = s.revision_id"
		" JOIN conversion_profile p ON p.id = r.profile_id WHERE r.number = 1");
	g_assert_cmpstr(facts_after, ==, facts_before);
	g_assert_cmpstr(samples_after, ==, samples_before);
	/* What their prediction read from the target is on the row now. */
	count = axis_test_db_query(db, "SELECT string_agg(coalesce(out_bit_depth::text, '-') || '/' ||"
		" coalesce(nominal_bitrate::text, '-'), ',' ORDER BY id) FROM estimate_sample");
	g_assert_cmpstr(count, ==, "10/-,8/-,-/32000,-/128000");
	g_clear_pointer(&count, g_free);
	/* The settings that became a profile's are gone; the encoder stays. */
	count = axis_test_db_query(db, "SELECT string_agg(key, ',' ORDER BY key) FROM setting");
	g_assert_cmpstr(count, ==, "estimate.encoder");

	/* The model reads the carried samples as before. */
	{
		g_autoptr(AxisStorage) storage = axis_storage_new(pool, NULL);
		g_autoptr(AxisEstimator) estimator = axis_estimator_new(pool, storage, NULL);
		AxisEstimateModel *model = axis_estimator_get_model(estimator, &error);
		AxisEstimateModel *reference = axis_estimate_model_new();
		AxisEstimateSample sample;
		guint i;

		g_assert_nonnull(model);
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
		sample.output_bitrate = 1500000;
		g_assert_true(axis_estimate_model_add_sample(reference, &sample));
		sample.target = "balanced";
		sample.output_bitrate = 1600000;
		g_assert_true(axis_estimate_model_add_sample(reference, &sample));
		memset(&sample, 0, sizeof(sample));
		sample.target = "audiobooks";
		sample.codec = AXIS_TCODEC_OPUS;
		sample.quality = 4;
		sample.part = "audio";
		sample.source_codec = "mp3";
		sample.source_bitrate = 128000;
		sample.sample_rate = 44100;
		sample.channels = 1;
		sample.bit_depth = 16;
		sample.seconds = 60.0;
		sample.output_bitrate = 33000;
		g_assert_true(axis_estimate_model_add_sample(reference, &sample));
		sample.target = "music_space_saver";
		sample.source_codec = "flac";
		sample.source_bitrate = 900000;
		sample.channels = 2;
		sample.output_bitrate = 130000;
		g_assert_true(axis_estimate_model_add_sample(reference, &sample));
		axis_estimate_model_finish(reference);
		for (i = 0; i < axis_estimate_model_n_cells(model); i++) {
			AxisEstimateCellInfo a;
			AxisEstimateCellInfo b;

			axis_estimate_model_cell_info(model, i, &a);
			axis_estimate_model_cell_info(reference, i, &b);
			g_assert_cmpuint(a.samples, ==, b.samples);
			g_assert_cmpfloat_with_epsilon(a.factor, b.factor, 1e-9);
		}
		axis_estimator_model_unref(model);
		axis_estimate_model_free(reference);
	}
	axis_db_pool_free(pool);
	axis_test_log_reset();
}

#define ADD(name, func) g_test_add("/profile/db/" name, Fixture, NULL, fixture_setup, func, fixture_teardown)
#define ADD_LIBRARY(name, func) g_test_add("/profile/db/" name, Fixture, "library", fixture_setup, func, \
	fixture_teardown)

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	ADD("builtins", test_builtins);
	ADD("builtin-read-only", test_builtin_read_only);
	ADD("revisions", test_revisions);
	ADD("lock", test_lock);
	ADD_LIBRARY("unusable", test_unusable);
	ADD_LIBRARY("estimates", test_estimates);
	ADD("samples-shared", test_samples_shared);
	ADD_LIBRARY("max-compat-threshold", test_max_compat_threshold);
	ADD_LIBRARY("preview", test_preview);
	ADD_LIBRARY("assignment", test_assignment);
	g_test_add_func("/profile/db/migration", test_migration);
	return g_test_run();
}
