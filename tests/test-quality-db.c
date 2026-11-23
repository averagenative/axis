/*
 * test-quality-db.c - quality profiles in the database: the built-ins
 * written from the code table and never changed or deleted by a person,
 * copies and saves (a stale form refused), a stored profile that is invalid
 * or from a newer version refused whole, and deletion refused while
 * anything uses the profile (a test module registers as a user, and the
 * default-profile setting counts)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <glib.h>
#include <string.h>

#include "axis-test-case.h"
#include "axis-test-db.h"
#include "axis-test-jobs.h"
#include "core/axis-settings.h"
#include "quality/axis-quality-store.h"

typedef struct
{
	AxisTestDb       *db;
	AxisDbPool       *pool;
	AxisSettings     *settings;
	AxisQualityStore *store;
} Fixture;

#define SKIP_WITHOUT_DB(fx) do { if ((fx)->db == NULL) return; } while (0)

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;

	memset(fx, 0, sizeof(*fx));
	axis_test_log_init();
	axis_test_log_reset();
	axis_test_log_allow(G_LOG_LEVEL_MESSAGE, "quality profiles:*");
	fx->db = axis_test_db_new();
	if (fx->db == NULL)
		return;
	fx->pool = axis_test_jobs_pool(fx->db, 4);
	fx->settings = axis_settings_new();
	g_assert_true(axis_settings_add_table(fx->settings, AXIS_QUALITY_MODULE_ID, axis_quality_store_settings(), &error));
	g_assert_true(axis_settings_load(fx->settings, NULL, &error));
	g_assert_no_error(error);
	fx->store = axis_quality_store_new(fx->pool, fx->settings);
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	if (fx->db == NULL) {
		axis_test_log_reset();
		return;
	}
	axis_quality_store_unref(fx->store);
	axis_settings_unref(fx->settings);
	axis_db_pool_free(fx->pool);
	axis_test_db_free(fx->db);
	axis_test_log_reset();
}

/* @sql is refused by the database. */
static void
refused(
	Fixture     *fx,
	const gchar *sql
){
	g_autoptr(GError) error = NULL;

	if (axis_db_pool_exec(fx->pool, sql, 0, NULL, NULL, &error))
		g_error("the database took: %s", sql);
	g_assert_nonnull(error);
}

static AxisQualityStored *
get(
	Fixture     *fx,
	const gchar *slug
){
	g_autoptr(GError) error = NULL;
	AxisQualityStored *stored = NULL;

	g_assert_true(axis_quality_store_get(fx->store, slug, &stored, &error));
	g_assert_no_error(error);
	return stored;
}

static gchar *
copy(
	Fixture     *fx,
	const gchar *source,
	const gchar *name
){
	g_autoptr(GError) error = NULL;
	gchar *slug = NULL;

	g_assert_true(axis_quality_store_copy(fx->store, source, name, "root", &slug, &error));
	g_assert_no_error(error);
	g_assert_nonnull(slug);
	return slug;
}

/* ── The built-ins ──────────────────────────────────────────────── */

static void
test_builtins(Fixture *fx, gconstpointer data)
{
	const AxisQualityBuiltin *b = axis_quality_builtins();
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) list = NULL;
	g_autofree gchar *count = NULL;
	guint changed = 99;
	guint i;

	SKIP_WITHOUT_DB(fx);

	/* The migration wrote the size limits of the built-ins as decimal
	 * megabytes and is never edited; the code has them in mebibytes, so the
	 * first sync brings all five up to date (as after an upgrade of Axis),
	 * and a second writes nothing. */
	g_assert_true(axis_quality_store_sync_builtins(fx->pool, &changed, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(changed, ==, AXIS_QUALITY_N_BUILTINS);
	g_assert_true(axis_quality_store_sync_builtins(fx->pool, &changed, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(changed, ==, 0);
	g_assert_true(axis_quality_store_list(fx->store, &list, &error));
	g_assert_cmpuint(list->len, ==, AXIS_QUALITY_N_BUILTINS);
	for (i = 0; i < AXIS_QUALITY_N_BUILTINS; i++) {
		const AxisQualityStored *s = g_ptr_array_index(list, i);
		AxisQualityProfile code;
		g_autofree gchar *text = NULL;

		g_assert_cmpstr(s->slug, ==, b[i].slug);
		g_assert_cmpstr(s->name, ==, b[i].name);
		g_assert_true(s->built_in);
		g_assert_true(s->usable);
		g_assert_null(s->unusable);
		g_assert_true(axis_quality_builtin_profile(b[i].slug, &code));
		text = axis_quality_profile_canonical(&code);
		g_assert_cmpstr(s->text, ==, text);
		g_assert_true(memcmp(&s->profile, &code, sizeof(code)) == 0);
	}

	/* A built-in whose stored row differs from the code is brought up to
	 * date (as after an upgrade of Axis), and only that one. */
	axis_test_db_exec(fx->db, "UPDATE quality_profile SET name = 'Old name', definition = replace(definition,"
		" '\"weight\":25', '\"weight\":30') WHERE slug = 'balanced'");
	g_assert_true(axis_quality_store_sync_builtins(fx->pool, &changed, &error));
	g_assert_cmpuint(changed, ==, 1);
	{
		g_autoptr(AxisQualityStored) s = get(fx, "balanced");

		g_assert_cmpstr(s->name, ==, "Balanced 1080p");
		g_assert_cmpint(s->profile.rules[0].weight, ==, 25);
		g_assert_cmpint(s->revision, ==, 3);	/* the first sync made it 2 */
	}
	/* A missing one is written again. */
	axis_test_db_exec(fx->db, "ALTER TABLE quality_profile DISABLE TRIGGER quality_profile_guarded");
	axis_test_db_exec(fx->db, "DELETE FROM quality_profile WHERE slug = 'uhd'");
	axis_test_db_exec(fx->db, "ALTER TABLE quality_profile ENABLE TRIGGER quality_profile_guarded");
	g_assert_true(axis_quality_store_sync_builtins(fx->pool, &changed, &error));
	g_assert_cmpuint(changed, ==, 1);
	count = axis_test_db_query(fx->db, "SELECT count(*) FROM quality_profile WHERE built_in");
	g_assert_cmpstr(count, ==, "5");
	g_assert_true(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "quality profiles: 1 built-in profile written*") >= 1);
}

/* What the database itself refuses. */
static void
test_refused(Fixture *fx, gconstpointer data)
{
	SKIP_WITHOUT_DB(fx);

	refused(fx, "DELETE FROM quality_profile WHERE slug = 'balanced'");
	refused(fx, "UPDATE quality_profile SET slug = 'other' WHERE slug = 'balanced'");
	refused(fx, "UPDATE quality_profile SET built_in = false WHERE slug = 'balanced'");
	refused(fx, "UPDATE quality_profile SET revision = 0 WHERE slug = 'balanced'");
	refused(fx, "TRUNCATE quality_profile");
	/* A user's profile is custom_<id>; a built-in never is. */
	refused(fx, "INSERT INTO quality_profile (slug, name, built_in, format, definition) VALUES ('mine', 'Mine', false,"
		" 1, '{\"format\":1}')");
	refused(fx, "INSERT INTO quality_profile (slug, name, built_in, format, definition) VALUES ('custom_9', 'Mine',"
		" true, 1, '{\"format\":1}')");
	/* Not JSON, not an object, or not saying its format. */
	refused(fx, "UPDATE quality_profile SET definition = '{' WHERE slug = 'balanced'");
	refused(fx, "UPDATE quality_profile SET definition = '[1]' WHERE slug = 'balanced'");
	refused(fx, "UPDATE quality_profile SET definition = '{\"format\":2}' WHERE slug = 'balanced'");
	refused(fx, "UPDATE quality_profile SET name = 'a\x01' WHERE slug = 'balanced'");
	refused(fx, "UPDATE quality_profile SET name = '' WHERE slug = 'balanced'");
}

/* ── Copies and saves ───────────────────────────────────────────── */

static void
test_copy_save(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *slug = NULL;
	g_autoptr(AxisQualityStored) mine = NULL;
	g_autoptr(AxisQualityStored) again = NULL;
	g_autoptr(GPtrArray) problems = axis_quality_problems_new();
	g_autoptr(GPtrArray) list = NULL;
	AxisQualityProfile edited;
	gboolean changed = TRUE;

	SKIP_WITHOUT_DB(fx);

	slug = copy(fx, "balanced", "My balanced");
	g_assert_true(g_str_has_prefix(slug, "custom_"));
	mine = get(fx, slug);
	g_assert_false(mine->built_in);
	g_assert_true(mine->usable);
	g_assert_cmpstr(mine->copied_from, ==, "balanced");
	g_assert_cmpstr(mine->updated_by, ==, "root");
	g_assert_cmpint(mine->revision, ==, 1);
	g_assert_cmpstr(mine->name, ==, "My balanced");

	/* Unchanged: nothing written. */
	g_assert_true(axis_quality_store_save(fx->store, slug, 1, "My balanced", &mine->profile, "root", &changed, problems,
		&error));
	g_assert_no_error(error);
	g_assert_false(changed);
	/* Changed: a new revision. */
	edited = mine->profile;
	edited.cutoff = AXIS_QUALITY_WEBDL_1080P;
	edited.allow_unknown = TRUE;
	g_assert_true(axis_quality_store_save(fx->store, slug, 1, "Mine, edited", &edited, "admin", &changed, problems,
		&error));
	g_assert_true(changed);
	again = get(fx, slug);
	g_assert_cmpint(again->revision, ==, 2);
	g_assert_cmpstr(again->name, ==, "Mine, edited");
	g_assert_cmpstr(again->updated_by, ==, "admin");
	g_assert_cmpint(again->profile.cutoff, ==, AXIS_QUALITY_WEBDL_1080P);
	g_assert_true(again->profile.allow_unknown);
	/* A form opened at revision 1 is refused now. */
	g_assert_false(axis_quality_store_save(fx->store, slug, 1, "Stale", &edited, "root", &changed, problems, &error));
	g_assert_error(error, AXIS_QUALITY_ERROR, AXIS_QUALITY_ERROR_CONFLICT);
	g_assert_false(changed);
	g_clear_error(&error);
	/* An invalid definition is refused with its problems. */
	edited.cutoff = AXIS_QUALITY_CAM;
	g_assert_false(axis_quality_store_save(fx->store, slug, 2, "Mine", &edited, "root", NULL, problems, &error));
	g_assert_error(error, AXIS_QUALITY_ERROR, AXIS_QUALITY_ERROR_INVALID);
	g_assert_nonnull(axis_quality_problems_find(problems, "cutoff"));
	g_clear_error(&error);
	/* A built-in never. */
	g_assert_false(axis_quality_store_save(fx->store, "balanced", 1, "Balanced", &mine->profile, "root", NULL, NULL,
		&error));
	g_assert_error(error, AXIS_QUALITY_ERROR, AXIS_QUALITY_ERROR_BUILT_IN);
	g_clear_error(&error);
	/* Nothing there. */
	g_assert_false(axis_quality_store_save(fx->store, "custom_999", 1, "X", &mine->profile, "root", NULL, NULL, &error));
	g_assert_error(error, AXIS_QUALITY_ERROR, AXIS_QUALITY_ERROR_NOT_FOUND);
	g_clear_error(&error);
	{
		gchar *none = NULL;

		g_assert_false(axis_quality_store_copy(fx->store, "nothing", "X", "root", &none, &error));
		g_assert_error(error, AXIS_QUALITY_ERROR, AXIS_QUALITY_ERROR_NOT_FOUND);
		g_assert_null(none);
		g_clear_error(&error);
	}

	/* The list: the built-ins first, then the copies. */
	{
		g_autofree gchar *second = copy(fx, "uhd", "My 4K");

		g_assert_true(axis_quality_store_list(fx->store, &list, &error));
		g_assert_cmpuint(list->len, ==, AXIS_QUALITY_N_BUILTINS + 2);
		g_assert_cmpstr(((AxisQualityStored *)g_ptr_array_index(list, 0))->slug, ==, "space_saver");
		g_assert_cmpstr(((AxisQualityStored *)g_ptr_array_index(list, AXIS_QUALITY_N_BUILTINS))->slug, ==, again->slug);
		g_assert_cmpstr(((AxisQualityStored *)g_ptr_array_index(list, AXIS_QUALITY_N_BUILTINS + 1))->slug, ==, second);
		g_assert_cmpstr(((AxisQualityStored *)g_ptr_array_index(list, AXIS_QUALITY_N_BUILTINS + 1))->copied_from, ==,
			"uhd");
	}
}

/* A stored profile that is invalid, or from a newer version, is refused
 * whole: nothing of it is used, it cannot be copied or edited, and it says
 * why. */
static void
test_unusable(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *slug = NULL;
	g_autofree gchar *sql = NULL;
	g_autofree gchar *other = NULL;
	g_autofree gchar *id = NULL;
	AxisQualityProfile profile;
	AxisQualityProfile zero;

	SKIP_WITHOUT_DB(fx);

	memset(&zero, 0, sizeof(zero));
	slug = copy(fx, "balanced", "Broken");
	id = axis_test_db_queryf(fx->db, "SELECT id FROM quality_profile WHERE slug = '%s'", slug);
	/* Valid JSON, valid format, but its cutoff is not one it allows. */
	sql = g_strdup_printf("UPDATE quality_profile SET definition = replace(definition, '\"cutoff\":\"bluray_1080p\"',"
		" '\"cutoff\":\"remux_2160p\"') WHERE slug = '%s'", slug);
	axis_test_db_exec(fx->db, sql);
	{
		g_autoptr(AxisQualityStored) s = get(fx, slug);
		AxisDbConn *conn = axis_db_pool_acquire(fx->pool, &error);

		g_assert_false(s->usable);
		g_assert_cmpstr(s->unusable, ==, "Its stored definition is not valid (cutoff): The cutoff is one of the "
			"allowed qualities. Nothing of it is used.");
		g_assert_true(memcmp(&s->profile, &zero, sizeof(zero)) == 0);
		memset(&profile, 0x5a, sizeof(profile));
		g_assert_false(axis_quality_store_load(conn, g_ascii_strtoll(id, NULL, 10), &profile, &error));
		g_assert_error(error, AXIS_QUALITY_ERROR, AXIS_QUALITY_ERROR_UNUSABLE);
		g_assert_true(memcmp(&profile, &zero, sizeof(zero)) == 0);
		g_clear_error(&error);
		axis_db_pool_release(fx->pool, conn);
		g_assert_false(axis_quality_store_copy(fx->store, slug, "Copy", "root", &other, &error));
		g_assert_error(error, AXIS_QUALITY_ERROR, AXIS_QUALITY_ERROR_UNUSABLE);
		g_clear_error(&error);
		/* Not even with a valid definition: it is copied from another. */
		g_assert_true(axis_quality_builtin_profile("balanced", &profile));
		g_assert_false(axis_quality_store_save(fx->store, slug, s->revision, "Fixed", &profile, "root", NULL, NULL,
			&error));
		g_assert_error(error, AXIS_QUALITY_ERROR, AXIS_QUALITY_ERROR_UNUSABLE);
		g_clear_error(&error);
	}
	/* From a newer version: format 2, members this one does not know. */
	g_free(sql);
	sql = g_strdup_printf("UPDATE quality_profile SET format = 2, definition = '{\"format\":2,\"ladder\":"
		"\"from the future\"}' WHERE slug = '%s'", slug);
	axis_test_db_exec(fx->db, sql);
	{
		g_autoptr(AxisQualityStored) s = get(fx, slug);
		AxisDbConn *conn = axis_db_pool_acquire(fx->pool, &error);

		g_assert_false(s->usable);
		g_assert_cmpstr(s->unusable, ==, "It was written by a newer version of Axis, in format 2; this version reads "
			"format 1. Nothing of it is used.");
		g_assert_false(axis_quality_store_load(conn, g_ascii_strtoll(id, NULL, 10), &profile, &error));
		g_assert_error(error, AXIS_QUALITY_ERROR, AXIS_QUALITY_ERROR_UNUSABLE);
		g_assert_nonnull(strstr(error->message, "newer version"));
		g_clear_error(&error);
		axis_db_pool_release(fx->pool, conn);
	}
	/* A usable one loads. */
	{
		AxisDbConn *conn = axis_db_pool_acquire(fx->pool, &error);
		g_autofree gchar *balanced = axis_test_db_query(fx->db, "SELECT id FROM quality_profile WHERE slug = 'balanced'");

		g_assert_true(axis_quality_store_load(conn, g_ascii_strtoll(balanced, NULL, 10), &profile, &error));
		g_assert_cmpint(profile.cutoff, ==, AXIS_QUALITY_BLURAY_1080P);
		g_assert_false(axis_quality_store_load(conn, 999999, &profile, &error));
		g_assert_error(error, AXIS_QUALITY_ERROR, AXIS_QUALITY_ERROR_NOT_FOUND);
		g_clear_error(&error);
		axis_db_pool_release(fx->pool, conn);
	}
	/* The check itself. */
	{
		gchar *why = NULL;

		g_assert_false(axis_quality_stored_check(1, "{\"format\":3}", &profile, &why));
		g_assert_nonnull(strstr(why, "format 3"));
		g_free(why);
		g_assert_false(axis_quality_stored_check(1, "{\"format\":1}", &profile, &why));
		g_assert_nonnull(strstr(why, "not valid"));
		g_free(why);
	}
}

/* ── In use ─────────────────────────────────────────────────────── */

static void
test_in_use(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *slug = NULL;
	g_autofree gchar *id = NULL;
	g_autofree gchar *insert = NULL;
	g_autoptr(GPtrArray) uses = NULL;
	g_autoptr(AxisQualityStored) gone = NULL;

	SKIP_WITHOUT_DB(fx);

	/* A module that refers to profiles, as the movies module will. */
	axis_test_db_exec(fx->db, "CREATE TABLE test_film (id bigint GENERATED ALWAYS AS IDENTITY, profile_id bigint"
		" NOT NULL REFERENCES quality_profile (id) ON DELETE RESTRICT)");
	g_assert_true(axis_quality_store_add_user(fx->store, "film", "films", "SELECT profile_id FROM test_film"));
	axis_test_log_allow(G_LOG_LEVEL_CRITICAL, "*is not one SELECT statement*");
	g_assert_false(axis_quality_store_add_user(fx->store, "bad", "bads", "SELECT 1; DROP TABLE quality_profile"));
	g_assert_false(axis_quality_store_add_user(fx->store, "bad", "bads", "DELETE FROM quality_profile"));
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_CRITICAL, "*is not one SELECT statement*"), ==, 2);

	slug = copy(fx, "space_saver", "Small");
	id = axis_test_db_queryf(fx->db, "SELECT id FROM quality_profile WHERE slug = '%s'", slug);
	insert = g_strdup_printf("INSERT INTO test_film (profile_id) VALUES (%s), (%s)", id, id);
	axis_test_db_exec(fx->db, insert);

	g_assert_true(axis_quality_store_uses(fx->store, slug, &uses, &error));
	g_assert_cmpuint(uses->len, ==, 1);
	g_assert_cmpstr(g_ptr_array_index(uses, 0), ==, "2 films");
	g_clear_pointer(&uses, g_ptr_array_unref);
	g_assert_false(axis_quality_store_delete(fx->store, slug, &uses, &error));
	g_assert_error(error, AXIS_QUALITY_ERROR, AXIS_QUALITY_ERROR_IN_USE);
	g_assert_cmpstr(error->message, ==, "The profile is in use: 2 films. Change what uses it first.");
	g_assert_cmpuint(uses->len, ==, 1);
	g_clear_error(&error);
	g_clear_pointer(&uses, g_ptr_array_unref);
	g_assert_nonnull((gone = get(fx, slug)));
	g_clear_pointer(&gone, axis_quality_stored_free);

	/* One film left. */
	axis_test_db_exec(fx->db, "DELETE FROM test_film WHERE id = (SELECT min(id) FROM test_film)");
	g_assert_true(axis_quality_store_uses(fx->store, slug, &uses, &error));
	g_assert_cmpstr(g_ptr_array_index(uses, 0), ==, "1 film");
	g_clear_pointer(&uses, g_ptr_array_unref);

	/* The default setting counts too. */
	axis_test_db_exec(fx->db, "DELETE FROM test_film");
	g_assert_true(axis_settings_set(fx->settings, AXIS_QUALITY_SETTING_DEFAULT, slug, &error));
	g_assert_no_error(error);
	g_assert_false(axis_quality_store_delete(fx->store, slug, NULL, &error));
	g_assert_error(error, AXIS_QUALITY_ERROR, AXIS_QUALITY_ERROR_IN_USE);
	g_assert_nonnull(strstr(error->message, "the default for new titles"));
	g_clear_error(&error);
	{
		g_autofree gchar *def = axis_quality_store_default_slug(fx->store);

		g_assert_cmpstr(def, ==, slug);
	}
	g_assert_true(axis_settings_set(fx->settings, AXIS_QUALITY_SETTING_DEFAULT, "balanced", &error));
	g_assert_false(axis_settings_set(fx->settings, AXIS_QUALITY_SETTING_DEFAULT, "Not a slug", &error));
	g_clear_error(&error);

	/* Nothing uses it now. */
	g_assert_true(axis_quality_store_delete(fx->store, slug, NULL, &error));
	g_assert_no_error(error);
	gone = get(fx, slug);
	g_assert_null(gone);
	/* A built-in is never deleted, used or not. */
	g_assert_false(axis_quality_store_delete(fx->store, "anything", NULL, &error));
	g_assert_error(error, AXIS_QUALITY_ERROR, AXIS_QUALITY_ERROR_BUILT_IN);
	g_clear_error(&error);
	g_assert_false(axis_quality_store_delete(fx->store, "custom_999", NULL, &error));
	g_assert_error(error, AXIS_QUALITY_ERROR, AXIS_QUALITY_ERROR_NOT_FOUND);
	g_clear_error(&error);
	/* A copy of a copy that is gone keeps its definition. */
	{
		g_autofree gchar *first = copy(fx, "balanced", "First");
		g_autofree gchar *second = copy(fx, first, "Second");
		g_autoptr(AxisQualityStored) s = NULL;

		g_assert_true(axis_quality_store_delete(fx->store, first, NULL, &error));
		s = get(fx, second);
		g_assert_null(s->copied_from);
		g_assert_true(s->usable);
	}
}

AXIS_TEST_DB_CASES(Fixture, fx->db != NULL)

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

#define ADD(path, func) AXIS_TEST_ADD_DB_CASE("/quality-db/" path, Fixture, NULL, func)
	ADD("builtins", test_builtins);
	ADD("refused", test_refused);
	ADD("copy-save", test_copy_save);
	ADD("unusable", test_unusable);
	ADD("in-use", test_in_use);
#undef ADD
	return g_test_run();
}
