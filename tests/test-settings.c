/*
 * test-settings.c - the settings store: units, declarations, validation,
 * defaults, the table, change notification, secrets and concurrency
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Tests that only need the store run in memory (load with no pool); those
 * that look at the table run against a throwaway PostgreSQL.
 */

#include <glib.h>
#include <string.h>

#include "axis-test-db.h"
#include "axis-test-jobs.h"
#include "core/axis-error.h"
#include "core/axis-module.h"
#include "core/axis-setting-units.h"
#include "core/axis-settings.h"

#define MIB (G_GINT64_CONSTANT(1) << 20)
#define GIB (G_GINT64_CONSTANT(1) << 30)
#define TIB (G_GINT64_CONSTANT(1) << 40)

static const gchar *const colours[] = { "red", "green", "blue", NULL };
static const gchar *const modes[] = { "fast", "slow", NULL };

static const AxisSettingInfo table[] = {
	{ "t.flag",   AXIS_SETTING_BOOLEAN, "Flag", "A switch.", 0, "true", 0, 0, 0, 0, NULL },
	{ "t.count",  AXIS_SETTING_INTEGER, "Count", NULL, 0, "5", 1, 10, 0, 0, NULL },
	{ "t.signed", AXIS_SETTING_INTEGER, "Signed", NULL, 0, "-3", G_MININT64, 0, 0, 0, NULL },
	{ "t.ratio",  AXIS_SETTING_DECIMAL, "Ratio", NULL, 0, "0.5", 0, 0, 0.0, 1.0, NULL },
	{ "t.name",   AXIS_SETTING_STRING, "Name", NULL, 0, "axis", 1, 16, 0, 0, NULL },
	{ "t.colour", AXIS_SETTING_CHOICE, "Colour", NULL, 0, "green", 0, 0, 0, 0, colours },
	{ "t.hosts",  AXIS_SETTING_STRING_LIST, "Hosts", NULL, 0, NULL, 0, 3, 0, 0, NULL },
	{ "t.keep",   AXIS_SETTING_DURATION, "Keep", NULL, 0, "7d", 3600, 30 * 86400, 0, 0, NULL },
	{ "t.quota",  AXIS_SETTING_BYTE_SIZE, "Quota", NULL, 0, "10GiB", MIB, TIB, 0, 0, NULL },
	{ "t.token",  AXIS_SETTING_STRING, "Token", NULL, AXIS_SETTING_SECRET, NULL, 0, 64, 0, 0, NULL },
	{ "t.mode",   AXIS_SETTING_CHOICE, "Mode", NULL, AXIS_SETTING_RESTART, "fast", 0, 0, 0, 0, modes },
	{ NULL, 0, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL }
};

/* ── Helpers ────────────────────────────────────────────────────── */

static AxisSettings *
new_store(AxisDbPool *pool)
{
	g_autoptr(GError) error = NULL;
	AxisSettings *settings = axis_settings_new();

	g_assert_true(axis_settings_add_table(settings, "t", table, &error));
	g_assert_no_error(error);
	g_assert_true(axis_settings_load(settings, pool, &error));
	g_assert_no_error(error);

	return settings;
}

static void
start_test(void)
{
	axis_test_log_init();
	axis_test_log_reset();
}

/* How many recorded log messages, at any level, contain @text. */
static guint
log_mentions(const gchar *text)
{
	g_autofree gchar *pattern = g_strdup_printf("*%s*", text);
	static const GLogLevelFlags levels[] = {
		G_LOG_LEVEL_DEBUG, G_LOG_LEVEL_INFO, G_LOG_LEVEL_MESSAGE, G_LOG_LEVEL_WARNING,
		G_LOG_LEVEL_CRITICAL, G_LOG_LEVEL_ERROR
	};
	guint count = 0;
	guint i;

	for (i = 0; i < G_N_ELEMENTS(levels); i++)
		count += axis_test_log_count(levels[i], pattern);

	return count;
}

static void
assert_refused(
	AxisSettings *settings,
	const gchar  *key,
	const gchar  *text
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *before = axis_settings_format(settings, key);

	if (axis_settings_set(settings, key, text, &error))
		g_error("'%s' was accepted for %s", text, key);
	g_assert_error(error, AXIS_ERROR, AXIS_ERROR_SETTING);
	g_assert_nonnull(strstr(error->message, key));
	if (before != NULL) {
		g_autofree gchar *after = axis_settings_format(settings, key);

		g_assert_cmpstr(after, ==, before);
	}
}

/* ── Units ──────────────────────────────────────────────────────── */

static void
test_duration_parse(void)
{
	static const struct
	{
		const gchar *text;
		gint64       seconds;
	} good[] = {
		{ "0", 0 }, { "90", 90 }, { "45s", 45 }, { "15m", 900 }, { "2h", 7200 },
		{ "7d", 604800 }, { "2w", 1209600 }, { "1h 30m", 5400 }, { "1h30m", 5400 },
		{ "  3D  ", 259200 }, { "1w 1d 1h 1m 1s", 694861 }, { "1H", 3600 },
		{ "999999999999999999", G_GINT64_CONSTANT(999999999999999999) }
	};
	static const gchar *const bad[] = {
		"", " ", "s", "h", "-1", "+1", "1.5h", "1x", "1 h 2", "1h 30", "1h1h", "30min",
		"1h,30m", "0x10", "1e3", "٣", "1h\n", "9223372036854775807", "99999999999999999999",
		"1000000000000000w", "1h-1m", "m5", NULL
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(good); i++) {
		gint64 seconds = -1;

		if (!axis_setting_parse_duration(good[i].text, &seconds))
			g_error("'%s' should be a duration", good[i].text);
		g_assert_cmpint(seconds, ==, good[i].seconds);
	}
	for (i = 0; bad[i] != NULL; i++) {
		gint64 seconds = 42;

		if (axis_setting_parse_duration(bad[i], &seconds))
			g_error("'%s' should not be a duration", bad[i]);
	}
	g_assert_false(axis_setting_parse_duration(NULL, NULL));
}

static void
test_duration_format(void)
{
	static const gint64 samples[] = { 0, 1, 59, 60, 61, 3599, 3600, 5400, 86400, 90061,
		7 * 86400, 30 * 86400, G_GINT64_CONSTANT(123456789012) };
	g_autofree gchar *text = NULL;
	guint i;

	for (i = 0; i < G_N_ELEMENTS(samples); i++) {
		gint64 back = -1;

		g_free(text);
		text = axis_setting_format_duration(samples[i]);
		g_assert_true(axis_setting_parse_duration(text, &back));
		g_assert_cmpint(back, ==, samples[i]);
	}

	g_free(text);
	text = axis_setting_format_duration(90061);
	g_assert_cmpstr(text, ==, "1d 1h 1m 1s");
	g_free(text);
	text = axis_setting_format_duration(0);
	g_assert_cmpstr(text, ==, "0s");
}

static void
test_byte_size_parse(void)
{
	static const struct
	{
		const gchar *text;
		gint64       bytes;
	} good[] = {
		{ "0", 0 }, { "512", 512 }, { "512B", 512 }, { "64K", 65536 }, { "64k", 65536 },
		{ "64KB", 65536 }, { "64KiB", 65536 }, { "10MiB", 10 * MIB }, { "2 GB", 2 * GIB },
		{ "  1T  ", TIB }, { "1P", TIB * 1024 }, { "1536M", 1536 * MIB }, { "8191P", 8191 * TIB * 1024 }
	};
	static const gchar *const bad[] = {
		"", " ", "K", "KiB", "-1", "1.5G", "1 .5G", "1Q", "1KBB", "1ki", "1Kib2", "10 MB!",
		"1GiBx", "99999999999999999999", "8192P", "9223372036854775807K", "1e3", "0x10",
		"1\nG", NULL
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(good); i++) {
		gint64 bytes = -1;

		if (!axis_setting_parse_byte_size(good[i].text, &bytes))
			g_error("'%s' should be a size", good[i].text);
		g_assert_cmpint(bytes, ==, good[i].bytes);
	}
	for (i = 0; bad[i] != NULL; i++) {
		if (axis_setting_parse_byte_size(bad[i], NULL))
			g_error("'%s' should not be a size", bad[i]);
	}
}

static void
test_byte_size_format(void)
{
	static const gint64 samples[] = { 0, 1, 1023, 1024, 1025, 10 * MIB, 1536 * MIB, 10 * GIB,
		TIB, TIB * 1024, G_GINT64_CONSTANT(123456789) };
	g_autofree gchar *text = NULL;
	guint i;

	for (i = 0; i < G_N_ELEMENTS(samples); i++) {
		gint64 back = -1;

		g_free(text);
		text = axis_setting_format_byte_size(samples[i]);
		g_assert_true(axis_setting_parse_byte_size(text, &back));
		g_assert_cmpint(back, ==, samples[i]);
	}

	g_free(text);
	text = axis_setting_format_byte_size(1536 * MIB);
	g_assert_cmpstr(text, ==, "1536MiB");
	g_free(text);
	text = axis_setting_format_byte_size(10 * GIB);
	g_assert_cmpstr(text, ==, "10GiB");
}

/* ── Types, limits and defaults (4.1) ───────────────────────────── */

/* Spec: Default when unset */
static void
test_defaults(void)
{
	g_autoptr(AxisSettings) settings = NULL;
	g_auto(GStrv) hosts = NULL;
	g_autofree gchar *name = NULL;
	g_autofree gchar *token = NULL;

	start_test();
	settings = new_store(NULL);

	g_assert_true(axis_settings_get_boolean(settings, "t.flag"));
	g_assert_cmpint(axis_settings_get_integer(settings, "t.count"), ==, 5);
	g_assert_cmpint(axis_settings_get_integer(settings, "t.signed"), ==, -3);
	g_assert_cmpfloat(axis_settings_get_decimal(settings, "t.ratio"), ==, 0.5);
	name = axis_settings_get_string(settings, "t.name");
	g_assert_cmpstr(name, ==, "axis");
	g_free(name);
	name = axis_settings_get_string(settings, "t.colour");
	g_assert_cmpstr(name, ==, "green");
	hosts = axis_settings_get_string_list(settings, "t.hosts");
	g_assert_cmpuint(g_strv_length(hosts), ==, 0);
	g_assert_cmpint(axis_settings_get_duration(settings, "t.keep"), ==, 7 * 86400);
	g_assert_cmpint(axis_settings_get_byte_size(settings, "t.quota"), ==, 10 * GIB);
	token = axis_settings_get_string(settings, "t.token");
	g_assert_cmpstr(token, ==, "");

	g_assert_false(axis_settings_is_set(settings, "t.count"));
}

/* Each type: valid values, spelled in the ways people spell them */
static void
test_valid_values(void)
{
	g_autoptr(AxisSettings) settings = NULL;
	g_autoptr(GError) error = NULL;
	g_auto(GStrv) hosts = NULL;
	g_autofree gchar *text = NULL;

	start_test();
	settings = new_store(NULL);

	g_assert_true(axis_settings_set(settings, "t.flag", " No ", &error));
	g_assert_false(axis_settings_get_boolean(settings, "t.flag"));
	g_assert_true(axis_settings_set(settings, "t.flag", "on", &error));
	g_assert_true(axis_settings_get_boolean(settings, "t.flag"));

	g_assert_true(axis_settings_set(settings, "t.count", "10", &error));
	g_assert_cmpint(axis_settings_get_integer(settings, "t.count"), ==, 10);
	g_assert_true(axis_settings_set(settings, "t.count", "1", &error));
	g_assert_cmpint(axis_settings_get_integer(settings, "t.count"), ==, 1);
	g_assert_true(axis_settings_set(settings, "t.signed", "-40000", &error));
	g_assert_cmpint(axis_settings_get_integer(settings, "t.signed"), ==, -40000);

	g_assert_true(axis_settings_set(settings, "t.ratio", "0.75", &error));
	g_assert_cmpfloat(axis_settings_get_decimal(settings, "t.ratio"), ==, 0.75);
	g_assert_true(axis_settings_set(settings, "t.ratio", "1", &error));
	g_assert_true(axis_settings_set(settings, "t.ratio", "0", &error));
	g_assert_true(axis_settings_set(settings, "t.ratio", "5e-1", &error));
	g_assert_cmpfloat(axis_settings_get_decimal(settings, "t.ratio"), ==, 0.5);

	g_assert_true(axis_settings_set(settings, "t.name", "  my axis ", &error));
	text = axis_settings_get_string(settings, "t.name");
	g_assert_cmpstr(text, ==, "my axis");
	g_free(text);

	g_assert_true(axis_settings_set(settings, "t.colour", "blue", &error));
	text = axis_settings_get_string(settings, "t.colour");
	g_assert_cmpstr(text, ==, "blue");

	g_assert_true(axis_settings_set(settings, "t.hosts", "a.example\r\n\n  b.example  \r\nc.example", &error));
	hosts = axis_settings_get_string_list(settings, "t.hosts");
	g_assert_cmpuint(g_strv_length(hosts), ==, 3);
	g_assert_cmpstr(hosts[1], ==, "b.example");
	g_free(text);
	text =axis_settings_format(settings, "t.hosts");
	g_assert_cmpstr(text, ==, "a.example\nb.example\nc.example");

	g_assert_true(axis_settings_set(settings, "t.keep", "36h", &error));
	g_assert_cmpint(axis_settings_get_duration(settings, "t.keep"), ==, 36 * 3600);
	g_free(text);
	text = axis_settings_format(settings, "t.keep");
	g_assert_cmpstr(text, ==, "1d 12h");

	g_assert_true(axis_settings_set(settings, "t.quota", "1536MiB", &error));
	g_assert_cmpint(axis_settings_get_byte_size(settings, "t.quota"), ==, 1536 * MIB);
	g_free(text);
	text = axis_settings_format(settings, "t.quota");
	g_assert_cmpstr(text, ==, "1536MiB");

	g_assert_true(axis_settings_is_set(settings, "t.quota"));
	g_free(text);
	text = axis_settings_format_default(settings, "t.quota");
	g_assert_cmpstr(text, ==, "10GiB");
	g_assert_no_error(error);
}

/* Spec: Invalid value rejected */
static void
test_invalid_values(void)
{
	static const struct
	{
		const gchar *key;
		const gchar *text;
	} bad[] = {
		{ "t.flag", "maybe" }, { "t.flag", "" },
		{ "t.count", "0" }, { "t.count", "11" }, { "t.count", "five" }, { "t.count", "1.5" },
		{ "t.count", "" }, { "t.count", "99999999999999999999" }, { "t.count", "-1" },
		{ "t.signed", "--1" }, { "t.signed", "1 2" },
		{ "t.ratio", "-0.1" }, { "t.ratio", "1.01" }, { "t.ratio", "nan" }, { "t.ratio", "inf" },
		{ "t.ratio", "1e400" }, { "t.ratio", "0x1" }, { "t.ratio", ".5" }, { "t.ratio", "1," },
		{ "t.name", "" }, { "t.name", "   " }, { "t.name", "12345678901234567" },
		{ "t.name", "tab\there" }, { "t.name", "\xff\xfe" },
		{ "t.colour", "purple" }, { "t.colour", "" }, { "t.colour", "RED" },
		{ "t.hosts", "a\nb\nc\nd" }, { "t.mode", "medium" },
		{ "t.keep", "30m" }, { "t.keep", "31d" }, { "t.keep", "7" }, { "t.keep", "a week" },
		{ "t.keep", "-1d" }, { "t.keep", "" },
		{ "t.quota", "1KiB" }, { "t.quota", "2TiB" }, { "t.quota", "ten" }, { "t.quota", "-5G" },
		{ "t.nothing", "1" }
	};
	g_autoptr(AxisSettings) settings = NULL;
	g_autoptr(GError) error = NULL;
	guint i;

	start_test();
	settings = new_store(NULL);

	/* Something stored first, so that "unchanged" means unchanged. */
	g_assert_true(axis_settings_set(settings, "t.count", "7", &error));
	g_assert_true(axis_settings_set(settings, "t.keep", "2d", &error));

	for (i = 0; i < G_N_ELEMENTS(bad); i++)
		assert_refused(settings, bad[i].key, bad[i].text);

	g_assert_cmpint(axis_settings_get_integer(settings, "t.count"), ==, 7);
	g_assert_cmpint(axis_settings_get_duration(settings, "t.keep"), ==, 2 * 86400);
	g_assert_false(axis_settings_is_set(settings, "t.ratio"));
}

/* AXIS_SETTING_ZERO_OFF: 0 is taken besides the limits, which hold for
 * everything else, and a refusal says both. */
static void
test_zero_switches_off(void)
{
	static const AxisSettingInfo zero_table[] = {
		{ "z.keep", AXIS_SETTING_DURATION, "Keep", NULL, AXIS_SETTING_ZERO_OFF, "30d", 7 * 86400, 365 * 86400,
		  0, 0, NULL },
		{ "z.count", AXIS_SETTING_INTEGER, "Count", NULL, AXIS_SETTING_ZERO_OFF, "5", 3, 10, 0, 0, NULL },
		{ "z.off", AXIS_SETTING_DURATION, "Off by default", NULL, AXIS_SETTING_ZERO_OFF, "0", 86400, 0, 0, 0,
		  NULL },
		{ NULL, 0, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL }
	};
	g_autoptr(AxisSettings) settings = axis_settings_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *shown = NULL;

	start_test();
	g_assert_true(axis_settings_add_table(settings, "z", zero_table, &error));
	g_assert_true(axis_settings_load(settings, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpint(axis_settings_get_duration(settings, "z.keep"), ==, 30 * 86400);
	g_assert_cmpint(axis_settings_get_duration(settings, "z.off"), ==, 0);

	g_assert_true(axis_settings_set(settings, "z.keep", "0", &error));
	g_assert_no_error(error);
	g_assert_cmpint(axis_settings_get_duration(settings, "z.keep"), ==, 0);
	shown = axis_settings_format(settings, "z.keep");
	g_assert_cmpstr(shown, ==, "0s");
	g_assert_true(axis_settings_set(settings, "z.keep", "7d", &error));
	g_assert_true(axis_settings_set(settings, "z.count", "0", &error));
	g_assert_no_error(error);

	g_assert_false(axis_settings_set(settings, "z.keep", "1d", &error));
	g_assert_error(error, AXIS_ERROR, AXIS_ERROR_SETTING);
	g_assert_nonnull(strstr(error->message, "must be 0 or at least 7d"));
	g_clear_error(&error);
	assert_refused(settings, "z.keep", "366d");
	assert_refused(settings, "z.keep", "-1d");
	assert_refused(settings, "z.count", "2");
	assert_refused(settings, "z.count", "11");
	g_assert_cmpint(axis_settings_get_duration(settings, "z.keep"), ==, 7 * 86400);
	g_assert_cmpint(axis_settings_get_integer(settings, "z.count"), ==, 0);
}

/* An item of a list must be sane on its own */
static void
test_list_items(void)
{
	g_autoptr(AxisSettings) settings = NULL;
	g_autofree gchar *long_line = g_strnfill(1025, 'x');

	start_test();
	settings = new_store(NULL);

	assert_refused(settings, "t.hosts", long_line);
	assert_refused(settings, "t.hosts", "ok\n\x01zzz");
}

/* ── Declarations ───────────────────────────────────────────────── */

typedef struct
{
	const gchar           *id;
	const AxisSettingInfo *settings;
} Decl;

static void
register_decl(
	AxisModuleRegistry *registry,
	const Decl         *decl
){
	g_autoptr(GError) error = NULL;
	AxisModuleInfo info;

	memset(&info, 0, sizeof(info));
	info.id = decl->id;
	info.name = decl->id;
	info.settings = decl->settings;
	g_assert_true(axis_module_registry_register(registry, &info, NULL, &error));
	g_assert_no_error(error);
}

/* Spec: Duplicate key refused */
static void
test_duplicate_key_refused(void)
{
	static const AxisSettingInfo first[] = {
		{ "shared.limit", AXIS_SETTING_INTEGER, "Limit", NULL, 0, "1", 0, 0, 0, 0, NULL },
		{ NULL, 0, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL }
	};
	static const AxisSettingInfo second[] = {
		{ "shared.other", AXIS_SETTING_INTEGER, "Other", NULL, 0, "1", 0, 0, 0, 0, NULL },
		{ "shared.limit", AXIS_SETTING_INTEGER, "Limit again", NULL, 0, "2", 0, 0, 0, 0, NULL },
		{ NULL, 0, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL }
	};
	static const AxisSettingInfo twice[] = {
		{ "own.same", AXIS_SETTING_BOOLEAN, "A", NULL, 0, NULL, 0, 0, 0, 0, NULL },
		{ "own.same", AXIS_SETTING_BOOLEAN, "B", NULL, 0, NULL, 0, 0, 0, 0, NULL },
		{ NULL, 0, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL }
	};
	g_autoptr(AxisModuleRegistry) registry = axis_module_registry_new();
	g_autoptr(AxisSettings) settings = axis_settings_new();
	g_autoptr(AxisSettings) again = axis_settings_new();
	g_autoptr(GError) error = NULL;
	Decl a = { "alpha", first };
	Decl b = { "beta", second };
	Decl c = { "gamma", twice };

	start_test();
	register_decl(registry, &a);
	register_decl(registry, &b);

	g_assert_false(axis_settings_add_modules(settings, registry, &error));
	g_assert_error(error, AXIS_ERROR, AXIS_ERROR_SETTING);
	g_assert_nonnull(strstr(error->message, "shared.limit"));
	g_assert_nonnull(strstr(error->message, "alpha"));
	g_assert_nonnull(strstr(error->message, "beta"));
	g_clear_error(&error);

	/* Nothing of the refused module was added. */
	g_assert_null(axis_settings_lookup(settings, "shared.other", NULL));

	/* A module that declares a key twice. */
	g_assert_false(axis_settings_add_table(again, "gamma", c.settings, &error));
	g_assert_error(error, AXIS_ERROR, AXIS_ERROR_SETTING);
	g_assert_nonnull(strstr(error->message, "own.same"));
}

static void
test_bad_declarations(void)
{
	static const gchar *const none[] = { NULL };
	static const AxisSettingInfo bad_key[] = {
		{ "NoDot", AXIS_SETTING_BOOLEAN, "x", NULL, 0, NULL, 0, 0, 0, 0, NULL },
		{ NULL, 0, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL }
	};
	static const AxisSettingInfo bad_default[] = {
		{ "m.count", AXIS_SETTING_INTEGER, "x", NULL, 0, "50", 1, 10, 0, 0, NULL },
		{ NULL, 0, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL }
	};
	static const AxisSettingInfo needs_default[] = {
		{ "m.count", AXIS_SETTING_INTEGER, "x", NULL, 0, NULL, 1, 10, 0, 0, NULL },
		{ NULL, 0, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL }
	};
	static const AxisSettingInfo secret_default[] = {
		{ "m.key", AXIS_SETTING_STRING, "x", NULL, AXIS_SETTING_SECRET, "hunter2", 0, 0, 0, 0, NULL },
		{ NULL, 0, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL }
	};
	static const AxisSettingInfo secret_number[] = {
		{ "m.key", AXIS_SETTING_INTEGER, "x", NULL, AXIS_SETTING_SECRET, NULL, 0, 0, 0, 0, NULL },
		{ NULL, 0, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL }
	};
	static const AxisSettingInfo no_choices[] = {
		{ "m.pick", AXIS_SETTING_CHOICE, "x", NULL, 0, NULL, 0, 0, 0, 0, none },
		{ NULL, 0, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL }
	};
	static const AxisSettingInfo no_label[] = {
		{ "m.flag", AXIS_SETTING_BOOLEAN, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL },
		{ NULL, 0, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL }
	};
	static const AxisSettingInfo inverted[] = {
		{ "m.keep", AXIS_SETTING_DURATION, "x", NULL, 0, "1h", 7200, 3600, 0, 0, NULL },
		{ NULL, 0, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL }
	};
	/* 0 switches off only what is a number. */
	static const AxisSettingInfo zero_off_text[] = {
		{ "m.name", AXIS_SETTING_STRING, "x", NULL, AXIS_SETTING_ZERO_OFF, "a", 0, 0, 0, 0, NULL },
		{ NULL, 0, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL }
	};
	const AxisSettingInfo *cases[] = { bad_key, bad_default, needs_default, secret_default,
		secret_number, no_choices, no_label, inverted, zero_off_text };
	g_autoptr(AxisSettings) settings = axis_settings_new();
	g_autoptr(GError) error = NULL;
	guint i;

	start_test();
	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		if (axis_settings_add_table(settings, "m", cases[i], &error))
			g_error("declaration %u was accepted", i);
		g_assert_error(error, AXIS_ERROR, AXIS_ERROR_SETTING);
		g_clear_error(&error);
	}

	/* The secret's default is not echoed in the complaint. */
	axis_settings_add_table(settings, "m", secret_default, &error);
	g_assert_null(strstr(error->message, "hunter2"));
	g_clear_error(&error);

	/* A good table still goes in, and not after the load. */
	g_assert_true(axis_settings_add_table(settings, "t", table, &error));
	g_assert_true(axis_settings_load(settings, NULL, &error));
	g_assert_false(axis_settings_add_table(settings, "later", bad_default, &error));
	g_clear_error(&error);
}

/* Spec: Defaults for older descriptions */
static void
test_module_without_settings(void)
{
	g_autoptr(AxisModuleRegistry) registry = axis_module_registry_new();
	g_autoptr(AxisSettings) settings = axis_settings_new();
	g_autoptr(GError) error = NULL;
	AxisModuleInfo info;
	const gchar *const *needs;

	start_test();
	memset(&info, 0, sizeof(info));
	info.id = "plain";
	info.name = "Plain";
	g_assert_true(axis_module_registry_register(registry, &info, NULL, &error));
	g_assert_no_error(error);

	g_assert_null(axis_module_registry_get_settings(registry, 0));
	needs = axis_module_registry_get_depends_on(registry, 0);
	g_assert_null(needs[0]);
	g_assert_false(axis_module_registry_is_required(registry, 0));
	g_assert_true(axis_module_registry_is_enabled(registry, "plain"));
	g_assert_null(axis_module_registry_get_description(registry, 0));

	g_assert_true(axis_settings_add_modules(settings, registry, &error));
	g_assert_no_error(error);
}

/* ── The table ──────────────────────────────────────────────────── */

typedef struct
{
	AxisTestDb *db;
	AxisDbPool *pool;
} Db;

static void
db_setup(Db *fx, gconstpointer data)
{
	memset(fx, 0, sizeof(*fx));
	axis_test_log_init();
	axis_test_log_reset();
	fx->db = axis_test_db_new();
	if (fx->db != NULL)
		fx->pool = axis_test_jobs_pool(fx->db, 4);
}

static void
db_teardown(Db *fx, gconstpointer data)
{
	if (fx->pool != NULL)
		axis_db_pool_free(fx->pool);
	if (fx->db != NULL)
		axis_test_db_free(fx->db);
	axis_test_log_reset();
}

#define NEEDS_DB(fx) do { if ((fx)->db == NULL) return; } while (0)

/* Writes are in the table and survive a restart (a new store) */
static void
test_values_survive_restart(Db *fx, gconstpointer data)
{
	g_autoptr(AxisSettings) first = NULL;
	g_autoptr(AxisSettings) second = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *stored = NULL;
	g_autofree gchar *name = NULL;
	g_auto(GStrv) hosts = NULL;

	NEEDS_DB(fx);
	first = new_store(fx->pool);
	g_assert_true(axis_settings_set(first, "t.count", "9", &error));
	g_assert_true(axis_settings_set(first, "t.name", "q\"b\\s <b>\xc3\xa9", &error));
	g_assert_true(axis_settings_set(first, "t.hosts", "a.example\nb.example", &error));
	g_assert_true(axis_settings_set(first, "t.keep", "2d", &error));
	g_assert_true(axis_settings_set(first, "t.ratio", "0.25", &error));
	g_assert_true(axis_settings_set(first, "t.flag", "false", &error));
	g_assert_no_error(error);

	stored = axis_test_db_query(fx->db, "SELECT value::text FROM setting WHERE key = 't.count'");
	g_assert_cmpstr(stored, ==, "9");
	g_free(stored);
	stored = axis_test_db_query(fx->db, "SELECT value::text FROM setting WHERE key = 't.keep'");
	g_assert_cmpstr(stored, ==, "172800");
	g_free(stored);
	stored = axis_test_db_query(fx->db, "SELECT value::text FROM setting WHERE key = 't.hosts'");
	g_assert_cmpstr(stored, ==, "[\"a.example\", \"b.example\"]");

	second = new_store(fx->pool);
	g_assert_cmpint(axis_settings_get_integer(second, "t.count"), ==, 9);
	g_assert_cmpint(axis_settings_get_duration(second, "t.keep"), ==, 2 * 86400);
	g_assert_cmpfloat(axis_settings_get_decimal(second, "t.ratio"), ==, 0.25);
	g_assert_false(axis_settings_get_boolean(second, "t.flag"));
	name = axis_settings_get_string(second, "t.name");
	g_assert_cmpstr(name, ==, "q\"b\\s <b>\xc3\xa9");
	hosts = axis_settings_get_string_list(second, "t.hosts");
	g_assert_cmpuint(g_strv_length(hosts), ==, 2);
	g_assert_true(axis_settings_is_set(second, "t.count"));
	g_assert_false(axis_settings_is_set(second, "t.quota"));
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_WARNING, "*"), ==, 0);
}

/* Spec: Undeclared row kept */
static void
test_undeclared_row_kept(Db *fx, gconstpointer data)
{
	g_autoptr(AxisSettings) settings = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *kept = NULL;

	NEEDS_DB(fx);
	axis_test_db_exec(fx->db,
		"INSERT INTO setting (key, value) VALUES ('old.feature', '{\"x\": [1, 2]}'), "
		"('gone.setting', '\"keep me\"')");
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*which no module declares*");

	settings = new_store(fx->pool);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_WARNING, "*'old.feature'*"), ==, 1);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_WARNING, "*'gone.setting'*"), ==, 1);

	/* Writing other settings leaves the rows alone. */
	g_assert_true(axis_settings_set(settings, "t.count", "3", &error));
	kept = axis_test_db_query(fx->db, "SELECT value::text FROM setting WHERE key = 'old.feature'");
	g_assert_cmpstr(kept, ==, "{\"x\": [1, 2]}");
	g_free(kept);
	kept = axis_test_db_query(fx->db, "SELECT count(*) FROM setting");
	g_assert_cmpstr(kept, ==, "3");
	g_assert_null(axis_settings_lookup(settings, "old.feature", NULL));
}

/* Spec: Stored value no longer valid */
static void
test_stored_value_no_longer_valid(Db *fx, gconstpointer data)
{
	g_autoptr(AxisSettings) settings = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *name = NULL;
	g_autofree gchar *stored = NULL;

	NEEDS_DB(fx);
	/* Out of range, wrong type, wrong type, not an allowed choice, valid. */
	axis_test_db_exec(fx->db,
		"INSERT INTO setting (key, value) VALUES ('t.count', '424242'), "
		"('t.flag', '\"secretive-flag-value\"'), ('t.keep', '{\"d\": 7}'), "
		"('t.colour', '\"ultraviolet-xyz\"'), ('t.name', '\"fine\"')");
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*is not valid; using the default*");

	settings = new_store(fx->pool);

	g_assert_cmpint(axis_settings_get_integer(settings, "t.count"), ==, 5);
	g_assert_true(axis_settings_get_boolean(settings, "t.flag"));
	g_assert_cmpint(axis_settings_get_duration(settings, "t.keep"), ==, 7 * 86400);
	g_free(name);
	name = axis_settings_get_string(settings, "t.colour");
	g_assert_cmpstr(name, ==, "green");
	g_free(name);
	name = axis_settings_get_string(settings, "t.name");
	g_assert_cmpstr(name, ==, "fine");

	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_WARNING, "*'t.count'*"), ==, 1);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_WARNING, "*'t.flag'*"), ==, 1);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_WARNING, "*'t.keep'*"), ==, 1);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_WARNING, "*'t.colour'*"), ==, 1);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_WARNING, "*'t.name'*"), ==, 0);
	/* Never the value. */
	g_assert_cmpuint(log_mentions("424242"), ==, 0);
	g_assert_cmpuint(log_mentions("secretive-flag-value"), ==, 0);
	g_assert_cmpuint(log_mentions("ultraviolet-xyz"), ==, 0);

	/* The bad rows are left alone until a valid value replaces one, even
	 * the default being written. */
	stored = axis_test_db_query(fx->db, "SELECT value::text FROM setting WHERE key = 't.count'");
	g_assert_cmpstr(stored, ==, "424242");
	g_assert_true(axis_settings_set(settings, "t.count", "5", &error));
	g_free(stored);
	stored = axis_test_db_query(fx->db, "SELECT value::text FROM setting WHERE key = 't.count'");
	g_assert_cmpstr(stored, ==, "5");
}

/* Nothing is stored when any value of a submission is refused */
static void
test_set_many_is_all_or_nothing(Db *fx, gconstpointer data)
{
	g_autoptr(AxisSettings) settings = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) problems = NULL;
	g_auto(GStrv) changed = NULL;
	g_autofree gchar *count = NULL;
	AxisSettingChange mixed[] = {
		{ "t.count", "4" }, { "t.keep", "2d" }, { "t.name", "" }, { "t.ratio", "7" }
	};
	AxisSettingChange good[] = { { "t.count", "4" }, { "t.keep", "2d" }, { "t.name", "axis" } };
	const AxisSettingProblem *problem;

	NEEDS_DB(fx);
	settings = new_store(fx->pool);

	g_assert_false(axis_settings_set_many(settings, mixed, G_N_ELEMENTS(mixed), &changed,
		&problems, &error));
	g_assert_error(error, AXIS_ERROR, AXIS_ERROR_SETTING);
	g_assert_null(changed);
	g_assert_nonnull(problems);
	g_assert_cmpuint(problems->len, ==, 2);
	problem = g_ptr_array_index(problems, 0);
	g_assert_cmpstr(problem->key, ==, "t.name");
	g_assert_nonnull(strstr(problem->message, "t.name"));
	g_assert_nonnull(strstr(problem->reason, "empty"));
	problem = g_ptr_array_index(problems, 1);
	g_assert_cmpstr(problem->key, ==, "t.ratio");

	count = axis_test_db_query(fx->db, "SELECT count(*) FROM setting");
	g_assert_cmpstr(count, ==, "0");
	g_assert_cmpint(axis_settings_get_integer(settings, "t.count"), ==, 5);
	g_assert_false(axis_settings_is_set(settings, "t.keep"));

	/* A value equal to the one in use is not a change. */
	g_clear_error(&error);
	g_assert_true(axis_settings_set_many(settings, good, G_N_ELEMENTS(good), &changed, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(g_strv_length(changed), ==, 2);
	g_assert_cmpstr(changed[0], ==, "t.count");
	g_assert_cmpstr(changed[1], ==, "t.keep");
	g_free(count);
	count = axis_test_db_query(fx->db, "SELECT count(*) FROM setting");
	g_assert_cmpstr(count, ==, "2");
}

/* ── Secrets (4.4, store level) ─────────────────────────────────── */

#define SECRET_VALUE "s3cr3t-api-key-0123456789"

/* Spec: Secret not logged; Secret cleared */
static void
test_secret_in_the_store(Db *fx, gconstpointer data)
{
	g_autoptr(AxisSettings) settings = NULL;
	g_autoptr(AxisSettings) other = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) problems = NULL;
	g_autofree gchar *token = NULL;
	g_autofree gchar *too_long = g_strnfill(65, 'k');
	g_autofree gchar *count = NULL;
	AxisSettingChange bad[] = { { "t.token", NULL }, { "t.count", "99" } };

	NEEDS_DB(fx);
	settings = new_store(fx->pool);

	g_assert_true(axis_settings_set(settings, "t.token", SECRET_VALUE, &error));
	g_assert_no_error(error);
	g_assert_true(axis_settings_is_set(settings, "t.token"));
	g_assert_null(axis_settings_format(settings, "t.token"));
	g_assert_null(axis_settings_format_default(settings, "t.token"));
	token = axis_settings_get_string(settings, "t.token");
	g_assert_cmpstr(token, ==, SECRET_VALUE);

	/* Refused (too long): the message names the setting, not the value. */
	g_assert_false(axis_settings_set(settings, "t.token", too_long, &error));
	g_assert_error(error, AXIS_ERROR, AXIS_ERROR_SETTING);
	g_assert_nonnull(strstr(error->message, "t.token"));
	g_assert_null(strstr(error->message, "kkkk"));
	g_clear_error(&error);
	bad[0].text = too_long;
	g_assert_false(axis_settings_set_many(settings, bad, 2, NULL, &problems, &error));
	g_assert_null(strstr(error->message, "kkkk"));
	g_assert_null(strstr(((AxisSettingProblem *)g_ptr_array_index(problems, 0))->message, "kkkk"));

	/* Control characters in a secret are refused without being quoted. */
	g_clear_error(&error);
	g_assert_false(axis_settings_set(settings, "t.token", SECRET_VALUE "\x01", &error));
	g_assert_null(strstr(error->message, SECRET_VALUE));
	g_clear_error(&error);

	g_assert_cmpuint(log_mentions(SECRET_VALUE), ==, 0);
	g_assert_cmpuint(log_mentions("kkkk"), ==, 0);

	/* Cleared: gone from the table, reads as not set. */
	g_assert_true(axis_settings_clear(settings, "t.token", &error));
	g_assert_no_error(error);
	g_assert_false(axis_settings_is_set(settings, "t.token"));
	g_free(token);
	token = axis_settings_get_string(settings, "t.token");
	g_assert_cmpstr(token, ==, "");
	count = axis_test_db_query(fx->db, "SELECT count(*) FROM setting WHERE key = 't.token'");
	g_assert_cmpstr(count, ==, "0");
	other = new_store(fx->pool);
	g_assert_false(axis_settings_is_set(other, "t.token"));

	/* Clearing what is not set is fine. */
	g_assert_true(axis_settings_clear(settings, "t.token", &error));
	g_assert_cmpuint(log_mentions(SECRET_VALUE), ==, 0);
}

/* A write that fails in the database reports and logs no value */
static void
test_failed_write_leaks_nothing(Db *fx, gconstpointer data)
{
	g_autoptr(AxisSettings) settings = NULL;
	g_autoptr(GError) error = NULL;

	NEEDS_DB(fx);
	settings = new_store(fx->pool);
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*could not store*");

	axis_test_db_exec(fx->db, "ALTER TABLE setting ADD CONSTRAINT no_secrets CHECK (key <> 't.token')");

	g_assert_false(axis_settings_set(settings, "t.token", SECRET_VALUE, &error));
	g_assert_nonnull(error);
	g_assert_null(strstr(error->message, SECRET_VALUE));
	g_assert_cmpuint(log_mentions(SECRET_VALUE), ==, 0);
	/* The cache did not move. */
	g_assert_false(axis_settings_is_set(settings, "t.token"));

	/* Other settings are not affected. */
	g_clear_error(&error);
	g_assert_true(axis_settings_set(settings, "t.count", "8", &error));
	g_assert_no_error(error);
	g_assert_cmpint(axis_settings_get_integer(settings, "t.count"), ==, 8);
}

/* ── Restart-required settings ──────────────────────────────────── */

static void
test_restart_setting(Db *fx, gconstpointer data)
{
	g_autoptr(AxisSettings) settings = NULL;
	g_autoptr(AxisSettings) restarted = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *mode = NULL;
	g_autofree gchar *shown = NULL;

	NEEDS_DB(fx);
	settings = new_store(fx->pool);
	g_assert_false(axis_settings_restart_pending(settings, "t.mode"));

	g_assert_true(axis_settings_set(settings, "t.mode", "slow", &error));
	g_assert_no_error(error);

	/* Stored and shown, but not in effect. */
	mode = axis_settings_get_string(settings, "t.mode");
	g_assert_cmpstr(mode, ==, "fast");
	shown = axis_settings_format(settings, "t.mode");
	g_assert_cmpstr(shown, ==, "slow");
	g_assert_true(axis_settings_restart_pending(settings, "t.mode"));
	g_assert_false(axis_settings_restart_pending(settings, "t.count"));

	restarted = new_store(fx->pool);
	g_free(mode);
	mode = axis_settings_get_string(restarted, "t.mode");
	g_assert_cmpstr(mode, ==, "slow");
	g_assert_false(axis_settings_restart_pending(restarted, "t.mode"));
}

/* ── Notification ───────────────────────────────────────────────── */

typedef struct
{
	GPtrArray *keys;
	GThread   *thread;
	guint      calls;
} Seen;

static void
on_changed(
	AxisSettings *settings,
	const gchar  *key,
	gpointer      user_data
){
	Seen *seen = user_data;

	seen->calls++;
	seen->thread = g_thread_self();
	g_ptr_array_add(seen->keys, g_strdup(key));
}

static gpointer
write_in_thread(gpointer data)
{
	AxisSettings *settings = data;

	g_assert_true(axis_settings_set(settings, "t.count", "6", NULL));
	return NULL;
}

/* Spec: Change visible */
static void
test_change_notification(void)
{
	g_autoptr(AxisSettings) settings = NULL;
	g_autoptr(GError) error = NULL;
	Seen one = { g_ptr_array_new_with_free_func(g_free), NULL, 0 };
	Seen all = { g_ptr_array_new_with_free_func(g_free), NULL, 0 };
	Seen mode = { g_ptr_array_new_with_free_func(g_free), NULL, 0 };
	guint id_one;
	guint id_all;
	guint id_mode;
	GThread *thread;

	start_test();
	settings = new_store(NULL);
	id_one = axis_settings_subscribe(settings, "t.count", on_changed, &one);
	id_all = axis_settings_subscribe(settings, NULL, on_changed, &all);
	id_mode = axis_settings_subscribe(settings, "t.mode", on_changed, &mode);

	/* From the main thread: seen, with the key and no value. */
	g_assert_true(axis_settings_set(settings, "t.count", "8", &error));
	axis_test_pump_for(20);
	g_assert_cmpuint(one.calls, ==, 1);
	g_assert_cmpstr(g_ptr_array_index(one.keys, 0), ==, "t.count");
	g_assert_cmpuint(all.calls, ==, 1);
	g_assert_cmpint(axis_settings_get_integer(settings, "t.count"), ==, 8);

	/* An unrelated key does not reach a subscriber to another. */
	g_assert_true(axis_settings_set(settings, "t.name", "other", &error));
	axis_test_pump_for(20);
	g_assert_cmpuint(one.calls, ==, 1);
	g_assert_cmpuint(all.calls, ==, 2);

	/* The same value again changes nothing and tells nobody. */
	g_assert_true(axis_settings_set(settings, "t.name", "other", &error));
	axis_test_pump_for(20);
	g_assert_cmpuint(all.calls, ==, 2);

	/* A restart setting is not announced. */
	g_assert_true(axis_settings_set(settings, "t.mode", "slow", &error));
	axis_test_pump_for(20);
	g_assert_cmpuint(mode.calls, ==, 0);

	/* From another thread: delivered on the main thread, and the new value
	 * is already readable. */
	thread = g_thread_new("writer", write_in_thread, settings);
	g_thread_join(thread);
	axis_test_pump_for(50);
	g_assert_cmpuint(one.calls, ==, 2);
	g_assert_true(one.thread == g_thread_self());
	g_assert_cmpint(axis_settings_get_integer(settings, "t.count"), ==, 6);

	/* Unsubscribed: not called again. */
	axis_settings_unsubscribe(settings, id_one);
	g_assert_true(axis_settings_set(settings, "t.count", "7", &error));
	axis_test_pump_for(20);
	g_assert_cmpuint(one.calls, ==, 2);
	g_assert_cmpuint(all.calls, ==, 4);

	axis_settings_unsubscribe(settings, id_all);
	axis_settings_unsubscribe(settings, id_mode);
	g_ptr_array_unref(one.keys);
	g_ptr_array_unref(all.keys);
	g_ptr_array_unref(mode.keys);
}

/* ── Concurrency (4.3) ──────────────────────────────────────────── */

typedef struct
{
	AxisSettings *settings;
	gint          stop;	/* atomic */
	gint          bad;	/* atomic: reads that saw something impossible */
	gint          reads;	/* atomic */
} Race;

static gpointer
reader_main(gpointer data)
{
	Race *race = data;

	while (!g_atomic_int_get(&race->stop)) {
		g_autofree gchar *name = axis_settings_get_string(race->settings, "t.name");
		g_auto(GStrv) hosts = axis_settings_get_string_list(race->settings, "t.hosts");
		gint64 count = axis_settings_get_integer(race->settings, "t.count");
		gint64 keep = axis_settings_get_duration(race->settings, "t.keep");
		gboolean name_ok = strcmp(name, "axis") == 0 || strcmp(name, "aaaa") == 0 ||
		                   strcmp(name, "bbbb") == 0;
		guint n = g_strv_length(hosts);

		/* Copies, so they are whole: all the same letter, or the default. */
		if (!name_ok || (n != 0 && n != 3) || (n == 3 && strcmp(hosts[0], hosts[2]) != 0) ||
		    count < 1 || count > 10 || (keep != 7 * 86400 && keep != 3600 && keep != 7200))
			g_atomic_int_inc(&race->bad);
		g_atomic_int_inc(&race->reads);
	}

	return NULL;
}

static void
test_concurrent_readers_during_writes(void)
{
	g_autoptr(AxisSettings) settings = NULL;
	g_autoptr(GError) error = NULL;
	Race race;
	GThread *readers[4];
	guint i;

	start_test();
	settings = new_store(NULL);
	memset(&race, 0, sizeof(race));
	race.settings = settings;

	for (i = 0; i < G_N_ELEMENTS(readers); i++)
		readers[i] = g_thread_new("reader", reader_main, &race);

	for (i = 0; i < 300; i++) {
		AxisSettingChange changes[] = {
			{ "t.name", i % 2 ? "aaaa" : "bbbb" },
			{ "t.hosts", i % 2 ? "x\nx\nx" : "y\ny\ny" },
			{ "t.count", i % 2 ? "3" : "4" },
			{ "t.keep", i % 2 ? "1h" : "2h" }
		};

		g_assert_true(axis_settings_set_many(settings, changes, G_N_ELEMENTS(changes), NULL, NULL,
			&error));
		g_assert_no_error(error);
		if (i % 50 == 0)
			axis_test_pump_for(1);
	}

	g_atomic_int_set(&race.stop, 1);
	for (i = 0; i < G_N_ELEMENTS(readers); i++)
		g_thread_join(readers[i]);

	g_assert_cmpint(g_atomic_int_get(&race.bad), ==, 0);
	g_assert_cmpint(g_atomic_int_get(&race.reads), >, 0);
}

/* ── Main ───────────────────────────────────────────────────────── */

#define DB_TEST(name, func) \
	g_test_add("/settings/" name, Db, NULL, db_setup, func, db_teardown)

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/settings/units/duration-parse", test_duration_parse);
	g_test_add_func("/settings/units/duration-format", test_duration_format);
	g_test_add_func("/settings/units/byte-size-parse", test_byte_size_parse);
	g_test_add_func("/settings/units/byte-size-format", test_byte_size_format);

	g_test_add_func("/settings/types/defaults", test_defaults);
	g_test_add_func("/settings/types/valid-values", test_valid_values);
	g_test_add_func("/settings/types/invalid-values", test_invalid_values);
	g_test_add_func("/settings/types/list-items", test_list_items);
	g_test_add_func("/settings/types/zero-switches-off", test_zero_switches_off);

	g_test_add_func("/settings/declarations/duplicate-key", test_duplicate_key_refused);
	g_test_add_func("/settings/declarations/bad", test_bad_declarations);
	g_test_add_func("/settings/declarations/older-modules", test_module_without_settings);

	DB_TEST("table/values-survive-restart", test_values_survive_restart);
	DB_TEST("table/undeclared-row-kept", test_undeclared_row_kept);
	DB_TEST("table/stored-value-no-longer-valid", test_stored_value_no_longer_valid);
	DB_TEST("table/set-many-all-or-nothing", test_set_many_is_all_or_nothing);
	DB_TEST("secrets/store", test_secret_in_the_store);
	DB_TEST("secrets/failed-write", test_failed_write_leaks_nothing);
	DB_TEST("restart/setting", test_restart_setting);

	g_test_add_func("/settings/notify/changes", test_change_notification);
	g_test_add_func("/settings/threads/readers-during-writes", test_concurrent_readers_during_writes);

	return g_test_run();
}
