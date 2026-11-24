/*
 * test-config.c - configuration defaults, file, environment, redaction
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <stdarg.h>
#include <string.h>

#include "core/axis-config.h"
#include "core/axis-error.h"

static void
assert_config_error(
	GError      *error,
	const gchar *needle1,
	const gchar *needle2
){
	g_assert_nonnull(error);
	g_assert_error(error, AXIS_ERROR, AXIS_ERROR_CONFIG);
	g_assert_nonnull(strstr(error->message, needle1));
	if (needle2 != NULL)
		g_assert_nonnull(strstr(error->message, needle2));
}

static void
assert_defaults(AxisConfig *config)
{
	g_assert_cmpstr(axis_config_get_host(config), ==, "127.0.0.1");
	g_assert_cmpuint(axis_config_get_port(config), ==, 7800);
	g_assert_null(axis_config_get_database_url(config));
	g_assert_cmpuint(axis_config_get_database_pool_size(config), ==, 10);
	g_assert_false(axis_config_has_database_pool_size(config));
	g_assert_cmpint(axis_config_get_log_level(config), ==, AXIS_LOG_LEVEL_INFO);
}

static void
test_defaults(void)
{
	g_autoptr(AxisConfig) config = axis_config_new();

	assert_defaults(config);
}

static void
test_file_values(void)
{
	g_autoptr(AxisConfig) config = axis_config_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	gint fd;

	fd = g_file_open_tmp("axis-config-XXXXXX.conf", &path, &error);
	g_assert_no_error(error);
	g_close(fd, NULL);
	g_file_set_contents(path,
		"[server]\nhost = 0.0.0.0\nport = 9000\n"
		"[database]\nurl = postgres://u@h/db\npool_size = 16\n"
		"[log]\nlevel = debug\n", -1, &error);
	g_assert_no_error(error);

	g_assert_true(axis_config_load_file(config, path, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(axis_config_get_host(config), ==, "0.0.0.0");
	g_assert_cmpuint(axis_config_get_port(config), ==, 9000);
	g_assert_cmpstr(axis_config_get_database_url(config), ==, "postgres://u@h/db");
	g_assert_cmpuint(axis_config_get_database_pool_size(config), ==, 16);
	g_assert_true(axis_config_has_database_pool_size(config));
	g_assert_cmpint(axis_config_get_log_level(config), ==, AXIS_LOG_LEVEL_DEBUG);

	g_unlink(path);
}

static void
test_partial_file_keeps_defaults(void)
{
	g_autoptr(AxisConfig) config = axis_config_new();
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_config_load_data(config, "[server]\nport = 9000\n", &error));
	g_assert_no_error(error);
	g_assert_cmpuint(axis_config_get_port(config), ==, 9000);
	g_assert_cmpstr(axis_config_get_host(config), ==, "127.0.0.1");
	g_assert_cmpint(axis_config_get_log_level(config), ==, AXIS_LOG_LEVEL_INFO);
}

static void
test_unknown_group(void)
{
	g_autoptr(AxisConfig) config = axis_config_new();
	g_autoptr(GError) error = NULL;

	g_assert_false(axis_config_load_data(config, "[bogus]\nx = 1\n", &error));
	assert_config_error(error, "bogus", NULL);
}

static void
test_unknown_key(void)
{
	g_autoptr(AxisConfig) config = axis_config_new();
	g_autoptr(GError) error = NULL;

	g_assert_false(axis_config_load_data(config, "[server]\nportt = 1\n", &error));
	assert_config_error(error, "server", "portt");
}

static void
test_syntax_error(void)
{
	g_autoptr(AxisConfig) config = axis_config_new();
	g_autoptr(GError) error = NULL;

	g_assert_false(axis_config_load_data(config, "this is not ini\n", &error));
	assert_config_error(error, "syntax", NULL);
}

static void
test_invalid_port(void)
{
	static const gchar *const bad[] = {
		"0", "70000", "65536", "-1", "+80", "80abc", "abc", "", "8.0",
		"99999999999999999999", NULL
	};
	guint i;

	for (i = 0; bad[i] != NULL; i++) {
		g_autoptr(AxisConfig) config = axis_config_new();
		g_autoptr(GError) error = NULL;
		g_autofree gchar *data = g_strdup_printf("[server]\nport = %s\n", bad[i]);

		g_assert_false(axis_config_load_data(config, data, &error));
		assert_config_error(error, "server.port", NULL);
		assert_defaults(config);
	}
}

static void
test_valid_port_bounds(void)
{
	g_autoptr(AxisConfig) config = axis_config_new();
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_config_load_data(config, "[server]\nport = 1\n", &error));
	g_assert_cmpuint(axis_config_get_port(config), ==, 1);
	g_assert_true(axis_config_load_data(config, "[server]\nport = 65535\n", &error));
	g_assert_cmpuint(axis_config_get_port(config), ==, 65535);
	g_assert_no_error(error);
}

static void
test_invalid_pool_size(void)
{
	static const gchar *const bad[] = {
		"0", "65", "100", "-1", "+8", "8.0", "1e1", "eight", "", "0x8",
		"99999999999999999999", NULL
	};
	guint i;

	for (i = 0; bad[i] != NULL; i++) {
		g_autoptr(AxisConfig) config = axis_config_new();
		g_autoptr(GError) error = NULL;
		g_autofree gchar *data = g_strdup_printf("[database]\npool_size = %s\n", bad[i]);

		g_assert_false(axis_config_load_data(config, data, &error));
		assert_config_error(error, "database.pool_size", NULL);
		assert_defaults(config);
	}
}

static void
test_valid_pool_size_bounds(void)
{
	g_autoptr(AxisConfig) config = axis_config_new();
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_config_load_data(config, "[database]\npool_size = 1\n", &error));
	g_assert_cmpuint(axis_config_get_database_pool_size(config), ==, 1);
	g_assert_true(axis_config_load_data(config, "[database]\npool_size = 64\n", &error));
	g_assert_cmpuint(axis_config_get_database_pool_size(config), ==, 64);
	g_assert_no_error(error);
}

static void
test_required_database_url(void)
{
	g_autoptr(AxisConfig) config = axis_config_new();
	g_autoptr(GError) error = NULL;

	g_assert_false(axis_config_check_required(config, &error));
	assert_config_error(error, "database URL is required", "AXIS_DATABASE_URL");
	g_clear_error(&error);

	g_assert_true(axis_config_load_data(config, "[database]\nurl = postgres://u@h/db\n", &error));
	g_assert_true(axis_config_check_required(config, &error));
	g_assert_no_error(error);
}

static void
test_invalid_level(void)
{
	static const gchar *const bad[] = { "verbose", "INFO", "", "warn", NULL };
	guint i;

	for (i = 0; bad[i] != NULL; i++) {
		g_autoptr(AxisConfig) config = axis_config_new();
		g_autoptr(GError) error = NULL;
		g_autofree gchar *data = g_strdup_printf("[log]\nlevel = %s\n", bad[i]);

		g_assert_false(axis_config_load_data(config, data, &error));
		assert_config_error(error, "log.level", NULL);
	}
}

static void
test_empty_host(void)
{
	g_autoptr(AxisConfig) config = axis_config_new();
	g_autoptr(GError) error = NULL;

	g_assert_false(axis_config_load_data(config, "[server]\nhost =\n", &error));
	assert_config_error(error, "server.host", NULL);
}

static void
test_missing_file(void)
{
	g_autoptr(AxisConfig) config = axis_config_new();
	g_autoptr(GError) error = NULL;

	g_assert_false(axis_config_load_file(config, "/nonexistent/axis.conf", &error));
	assert_config_error(error, "/nonexistent/axis.conf", NULL);
}

static void
test_failed_load_leaves_config_unchanged(void)
{
	g_autoptr(AxisConfig) config = axis_config_new();
	g_autoptr(GError) error = NULL;

	/* The first settings are valid; the last one is not. */
	g_assert_false(axis_config_load_data(config,
		"[server]\nhost = example.org\nport = 9000\n"
		"[database]\nurl = postgres://x@y/z\n"
		"[log]\nlevel = nonsense\n", &error));
	assert_config_error(error, "log.level", NULL);
	assert_defaults(config);
	g_clear_error(&error);

	/* A valid pool size before an invalid one is not applied either. */
	g_assert_false(axis_config_load_data(config,
		"[database]\npool_size = 4\n[server]\nport = 0\n", &error));
	assert_defaults(config);
	g_clear_error(&error);

	/* Same for an unknown key after valid ones. */
	g_assert_false(axis_config_load_data(config,
		"[server]\nport = 9000\nbogus = 1\n", &error));
	assert_defaults(config);
}

static gchar **
make_env(const gchar *first, ...)
{
	gchar **env = g_new0(gchar *, 1);
	const gchar *name = first;
	va_list ap;

	va_start(ap, first);
	while (name != NULL) {
		const gchar *value = va_arg(ap, const gchar *);

		env = g_environ_setenv(env, name, value, TRUE);
		name = va_arg(ap, const gchar *);
	}
	va_end(ap);

	return env;
}

static void
test_env_overrides(void)
{
	g_autoptr(AxisConfig) config = axis_config_new();
	g_autoptr(GError) error = NULL;
	g_auto(GStrv) env = make_env(
		"AXIS_LISTEN_HOST", "10.0.0.5",
		"AXIS_LISTEN_PORT", "9100",
		"AXIS_DATABASE_URL", "postgres://a@b/c",
		"AXIS_DATABASE_POOL_SIZE", "32",
		"AXIS_LOG_LEVEL", "error", NULL);

	g_assert_true(axis_config_apply_environment(config, env, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(axis_config_get_host(config), ==, "10.0.0.5");
	g_assert_cmpuint(axis_config_get_port(config), ==, 9100);
	g_assert_cmpstr(axis_config_get_database_url(config), ==, "postgres://a@b/c");
	g_assert_cmpuint(axis_config_get_database_pool_size(config), ==, 32);
	g_assert_cmpint(axis_config_get_log_level(config), ==, AXIS_LOG_LEVEL_ERROR);
}

static void
test_env_wins_over_file(void)
{
	g_autoptr(AxisConfig) config = axis_config_new();
	g_autoptr(GError) error = NULL;
	g_auto(GStrv) env = make_env("AXIS_LISTEN_PORT", "9100", NULL);

	g_assert_true(axis_config_load_data(config, "[server]\nport = 9000\nhost = h.example\n", &error));
	g_assert_cmpuint(axis_config_get_port(config), ==, 9000);

	g_assert_true(axis_config_apply_environment(config, env, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(axis_config_get_port(config), ==, 9100);
	/* Settings with no override keep the file's value. */
	g_assert_cmpstr(axis_config_get_host(config), ==, "h.example");
}

static void
test_env_invalid_value(void)
{
	static const gchar *const cases[][2] = {
		{ "AXIS_LISTEN_PORT", "80abc" },
		{ "AXIS_LISTEN_PORT", "70000" },
		{ "AXIS_LISTEN_PORT", "80 " },
		{ "AXIS_LISTEN_PORT", "-1" },
		{ "AXIS_DATABASE_POOL_SIZE", "0" },
		{ "AXIS_DATABASE_POOL_SIZE", "65" },
		{ "AXIS_DATABASE_POOL_SIZE", "many" },
		{ "AXIS_LOG_LEVEL", "loud" }
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		g_autoptr(AxisConfig) config = axis_config_new();
		g_autoptr(GError) error = NULL;
		g_auto(GStrv) env = make_env(
			"AXIS_LISTEN_HOST", "10.0.0.5", cases[i][0], cases[i][1], NULL);

		g_assert_false(axis_config_apply_environment(config, env, &error));
		assert_config_error(error, cases[i][0], NULL);
		/* All or nothing: the valid host was not applied either. */
		assert_defaults(config);
	}
}

static void
test_env_empty_ignored(void)
{
	g_autoptr(AxisConfig) config = axis_config_new();
	g_autoptr(GError) error = NULL;
	g_auto(GStrv) env = make_env(
		"AXIS_LISTEN_HOST", "", "AXIS_LISTEN_PORT", "",
		"AXIS_DATABASE_URL", "", "AXIS_DATABASE_POOL_SIZE", "",
		"AXIS_LOG_LEVEL", "", NULL);

	g_assert_true(axis_config_load_data(config, "[server]\nport = 9000\n", &error));
	g_assert_true(axis_config_apply_environment(config, env, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(axis_config_get_port(config), ==, 9000);
	g_assert_cmpstr(axis_config_get_host(config), ==, "127.0.0.1");
}

/*
 * The portal listener (add-request-portal): off by default, and on the main
 * listener's address until it is given one of its own, in the file or in the
 * environment (server.portal_listen_host, AXIS_PORTAL_LISTEN_HOST); an empty
 * one is refused.
 */
static void
test_portal_listener(void)
{
	g_autoptr(AxisConfig) config = axis_config_new();
	g_autoptr(AxisConfig) refused = axis_config_new();
	g_autoptr(GError) error = NULL;
	g_auto(GStrv) env = make_env("AXIS_PORTAL_LISTEN_HOST", "10.0.0.7", NULL);
	g_autofree gchar *text = NULL;

	g_assert_cmpuint(axis_config_get_portal_port(config), ==, 0);
	g_assert_cmpstr(axis_config_get_portal_host(config), ==, "127.0.0.1");
	g_assert_true(axis_config_load_data(config, "[server]\nhost = 0.0.0.0\nportal_listen_port = 7801\n", &error));
	g_assert_no_error(error);
	g_assert_cmpuint(axis_config_get_portal_port(config), ==, 7801);
	g_assert_cmpstr(axis_config_get_portal_host(config), ==, "0.0.0.0");
	g_assert_true(axis_config_load_data(config, "[server]\nhost = 0.0.0.0\nportal_listen_host = 10.0.0.9\n"
		"portal_listen_port = 7801\n", &error));
	g_assert_no_error(error);
	g_assert_cmpstr(axis_config_get_portal_host(config), ==, "10.0.0.9");
	g_assert_cmpstr(axis_config_get_host(config), ==, "0.0.0.0");
	g_assert_true(axis_config_apply_environment(config, env, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(axis_config_get_portal_host(config), ==, "10.0.0.7");
	g_assert_cmpstr(axis_config_get_host(config), ==, "0.0.0.0");
	text = axis_config_describe(config);
	g_assert_nonnull(strstr(text, "portal_listen_host=10.0.0.7 portal_listen_port=7801"));

	g_assert_false(axis_config_load_data(refused, "[server]\nportal_listen_host =\n", &error));
	assert_config_error(error, "portal_listen_host", NULL);
	assert_defaults(refused);
	g_assert_cmpstr(axis_config_get_portal_host(refused), ==, "127.0.0.1");
}

#define FILE_SECRET "s3cr3t-from-a-file"

/* Writes @contents to a new temporary file and returns its path. */
static gchar *
write_temp(const gchar *contents)
{
	g_autoptr(GError) error = NULL;
	gchar *path = NULL;
	gint fd;

	fd = g_file_open_tmp("axis-config-url-XXXXXX", &path, &error);
	g_assert_no_error(error);
	g_close(fd, NULL);
	g_file_set_contents(path, contents, -1, &error);
	g_assert_no_error(error);

	return path;
}

/* Spec (daemon-core): Read from file -- a URL, with and without the
 * newline an editor or "echo" adds, and with CRLF. */
static void
test_url_file_read(void)
{
	static const gchar *const contents[] = {
		"postgres://u:" FILE_SECRET "@h/db\n",
		"postgres://u:" FILE_SECRET "@h/db",
		"postgres://u:" FILE_SECRET "@h/db\r\n",
		"\n  postgres://u:" FILE_SECRET "@h/db  \n\n"
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(contents); i++) {
		g_autoptr(AxisConfig) config = axis_config_new();
		g_autoptr(GError) error = NULL;
		g_autofree gchar *path = write_temp(contents[i]);
		g_auto(GStrv) env = make_env("AXIS_DATABASE_URL_FILE", path, NULL);

		g_assert_true(axis_config_apply_environment(config, env, &error));
		g_assert_no_error(error);
		g_assert_cmpstr(axis_config_get_database_url(config), ==,
			"postgres://u:" FILE_SECRET "@h/db");
		g_assert_true(axis_config_check_required(config, &error));
		g_unlink(path);
	}
}

/* The file's URL replaces one from the configuration file, as
 * AXIS_DATABASE_URL does; an empty variable is not set. */
static void
test_url_file_wins_over_config_file(void)
{
	g_autoptr(AxisConfig) config = axis_config_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = write_temp("postgres://from-file@h/db\n");
	g_auto(GStrv) env = make_env("AXIS_DATABASE_URL_FILE", path,
		"AXIS_DATABASE_URL", "", NULL);

	g_assert_true(axis_config_load_data(config, "[database]\nurl = postgres://from-conf@h/db\n",
		&error));
	g_assert_true(axis_config_apply_environment(config, env, &error));
	g_assert_cmpstr(axis_config_get_database_url(config), ==, "postgres://from-file@h/db");
	g_unlink(path);

	g_clear_pointer(&env, g_strfreev);
	env = make_env("AXIS_DATABASE_URL_FILE", "", NULL);
	g_assert_true(axis_config_apply_environment(config, env, &error));
	g_assert_cmpstr(axis_config_get_database_url(config), ==, "postgres://from-file@h/db");
}

/* Spec (daemon-core): Both set -- an error naming both variables, and the
 * configuration is left as it was. */
static void
test_url_file_and_url_both_set(void)
{
	g_autoptr(AxisConfig) config = axis_config_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = write_temp("postgres://u:" FILE_SECRET "@h/db\n");
	g_auto(GStrv) env = make_env("AXIS_DATABASE_URL", "postgres://other@h/db",
		"AXIS_DATABASE_URL_FILE", path, "AXIS_LISTEN_PORT", "9100", NULL);

	g_assert_false(axis_config_apply_environment(config, env, &error));
	assert_config_error(error, "AXIS_DATABASE_URL", "AXIS_DATABASE_URL_FILE");
	g_assert_null(strstr(error->message, FILE_SECRET));
	assert_defaults(config);
	g_unlink(path);
}

/* Spec (daemon-core): Unreadable file -- the error names the path, and
 * not the content; an empty or oversized file is refused the same way. */
static void
test_url_file_unreadable(void)
{
	g_autofree gchar *empty = write_temp(" \n");
	g_autofree gchar *large = NULL;
	g_autofree gchar *dir = g_dir_make_tmp("axis-config-dir-XXXXXX", NULL);
	g_autofree gchar *filler = g_strnfill(AXIS_CONFIG_MAX_URL_FILE_SIZE + 1, 'x');
	g_autofree gchar *large_contents = g_strconcat("postgres://u:", FILE_SECRET, "@h/", filler, NULL);
	const gchar *paths[4];
	guint i;

	large = write_temp(large_contents);
	paths[0] = "/nonexistent/axis/url";
	paths[1] = dir;
	paths[2] = empty;
	paths[3] = large;

	for (i = 0; i < G_N_ELEMENTS(paths); i++) {
		g_autoptr(AxisConfig) config = axis_config_new();
		g_autoptr(GError) error = NULL;
		g_auto(GStrv) env = make_env("AXIS_DATABASE_URL_FILE", paths[i], NULL);

		g_assert_false(axis_config_apply_environment(config, env, &error));
		assert_config_error(error, "AXIS_DATABASE_URL_FILE", paths[i]);
		g_assert_null(strstr(error->message, FILE_SECRET));
		g_assert_null(axis_config_get_database_url(config));
	}

	g_unlink(empty);
	g_unlink(large);
	g_rmdir(dir);
}

/* The description that gets logged never has the file's password. */
static void
test_url_file_described_without_password(void)
{
	g_autoptr(AxisConfig) config = axis_config_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = write_temp("postgres://u:" FILE_SECRET "@h/db\n");
	g_auto(GStrv) env = make_env("AXIS_DATABASE_URL_FILE", path, NULL);
	g_autofree gchar *description = NULL;

	g_assert_true(axis_config_apply_environment(config, env, &error));
	description = axis_config_describe(config);
	g_assert_null(strstr(description, FILE_SECRET));
	g_assert_nonnull(strstr(description, "postgres://u:****@h/db"));
	g_unlink(path);
}

static void
assert_redacted(
	const gchar *input,
	const gchar *expected,
	const gchar *secret
){
	g_autofree gchar *out = axis_config_redact_url(input);

	g_assert_cmpstr(out, ==, expected);
	if (secret != NULL)
		g_assert_null(strstr(out, secret));
}

static void
test_redact_url_form(void)
{
	assert_redacted("postgres://axis:s3cret@db.example:5432/axis",
		"postgres://axis:****@db.example:5432/axis", "s3cret");
	assert_redacted("postgresql://axis:s3cret@db/axis?sslmode=require",
		"postgresql://axis:****@db/axis?sslmode=require", "s3cret");
	/* unescaped '@' inside the password */
	assert_redacted("postgres://axis:p@ss@db/axis",
		"postgres://axis:****@db/axis", "p@ss");
	/* no password: unchanged */
	assert_redacted("postgres://axis@db/axis", "postgres://axis@db/axis", NULL);
	assert_redacted("postgres://db/axis", "postgres://db/axis", NULL);
}

static void
test_redact_query_form(void)
{
	assert_redacted("postgres://db/axis?user=axis&password=s3cret&sslmode=require",
		"postgres://db/axis?user=axis&password=****&sslmode=require", "s3cret");
	assert_redacted("postgres://db/axis?Password=s3cret",
		"postgres://db/axis?Password=****", "s3cret");
	assert_redacted("postgres://axis:one@db/axis?password=two#frag",
		"postgres://axis:****@db/axis?password=****#frag", "two");
}

static void
test_redact_keyword_form(void)
{
	assert_redacted("host=db port=5432 password=s3cret dbname=axis",
		"host=db port=5432 password=**** dbname=axis", "s3cret");
	assert_redacted("host=db password='s3 cret' dbname=axis",
		"host=db password=**** dbname=axis", "cret");
	assert_redacted("host=db password='it\\'s secret' dbname=axis",
		"host=db password=**** dbname=axis", "secret");
	assert_redacted("host=db password = s3cret",
		"host=db password =****", "s3cret");
}

static void
test_redact_garbage(void)
{
	assert_redacted("hunter2", "<redacted>", "hunter2");
	assert_redacted("not a url at all", "<redacted>", NULL);
	assert_redacted("host=db password='unterminated", "<redacted>", "unterminated");
	assert_redacted("postgres://u:p w@db/x", "<redacted>", "p w");
	assert_redacted("://u:secret@db", "<redacted>", "secret");
}

static void
test_redact_null_and_empty(void)
{
	g_assert_null(axis_config_redact_url(NULL));
	assert_redacted("", "", NULL);
}

static void
test_describe_never_contains_password(void)
{
	static const gchar *const urls[] = {
		"postgres://axis:s3cretpw@db/axis",
		"postgres://db/axis?password=s3cretpw",
		"host=db password=s3cretpw",
		"s3cretpw",
		NULL
	};
	guint i;

	for (i = 0; urls[i] != NULL; i++) {
		g_autoptr(AxisConfig) config = axis_config_new();
		g_autoptr(GError) error = NULL;
		g_auto(GStrv) env = make_env("AXIS_DATABASE_URL", urls[i], NULL);
		g_autofree gchar *text = NULL;

		g_assert_true(axis_config_apply_environment(config, env, &error));
		text = axis_config_describe(config);
		g_assert_nonnull(text);
		g_assert_null(strstr(text, "s3cretpw"));
		g_assert_nonnull(strstr(text, "127.0.0.1"));
		g_assert_nonnull(strstr(text, "7800"));
	}
}

static void
test_describe_without_database(void)
{
	g_autoptr(AxisConfig) config = axis_config_new();
	g_autofree gchar *text = axis_config_describe(config);

	g_assert_nonnull(strstr(text, "log_level=info"));
	g_assert_nonnull(strstr(text, "(none)"));
	g_assert_nonnull(strstr(text, "database_pool_size=10"));
}

/* The start-up line shows the pool size in use, not the configured default. */
static void
test_describe_pool_size_used(void)
{
	g_autoptr(AxisConfig) config = axis_config_new();
	g_autofree gchar *text = NULL;

	axis_config_set_database_pool_size_used(config, 15);
	text = axis_config_describe(config);
	g_assert_nonnull(strstr(text, "database_pool_size=15 "));
	g_assert_null(strstr(text, "database_pool_size=10"));
}

static guint
concurrency_of(
	AxisConfig  *config,
	const gchar *queue
){
	guint n = 999;

	g_assert_true(axis_config_get_jobs_concurrency(config, queue, &n));
	return n;
}

static void
test_jobs_defaults(void)
{
	g_autoptr(AxisConfig) config = axis_config_new();
	g_auto(GStrv) queues = axis_config_list_jobs_concurrency(config);
	g_autofree gchar *text = axis_config_describe(config);
	guint n = 7;

	g_assert_cmpuint(axis_config_get_jobs_grace_period(config), ==, 30);
	g_assert_cmpuint(g_strv_length(queues), ==, 0);
	g_assert_false(axis_config_get_jobs_concurrency(config, "scan", &n));
	g_assert_cmpuint(n, ==, 7);
	g_assert_nonnull(strstr(text, "jobs_concurrency=(default) jobs_grace_period=30"));
}

static void
test_jobs_file_values(void)
{
	g_autoptr(AxisConfig) config = axis_config_new();
	g_autoptr(GError) error = NULL;
	g_auto(GStrv) queues = NULL;
	g_autofree gchar *text = NULL;
	guint n = 0;

	g_assert_true(axis_config_load_data(config,
		"[jobs]\nconcurrency = scan:2, probe:4 ,transcode:0,file-ops.slow:64\ngrace_period = 0\n",
		&error));
	g_assert_no_error(error);
	g_assert_cmpuint(axis_config_get_jobs_grace_period(config), ==, 0);

	queues = axis_config_list_jobs_concurrency(config);
	g_assert_cmpuint(g_strv_length(queues), ==, 4);
	g_assert_cmpstr(queues[0], ==, "scan");
	g_assert_cmpstr(queues[1], ==, "probe");
	g_assert_cmpstr(queues[2], ==, "transcode");
	g_assert_cmpstr(queues[3], ==, "file-ops.slow");
	g_assert_cmpuint(concurrency_of(config, "scan"), ==, 2);
	g_assert_cmpuint(concurrency_of(config, "probe"), ==, 4);
	g_assert_cmpuint(concurrency_of(config, "transcode"), ==, 0);
	g_assert_cmpuint(concurrency_of(config, "file-ops.slow"), ==, 64);
	/* A prefix of a listed name is not that name. */
	g_assert_false(axis_config_get_jobs_concurrency(config, "sca", &n));
	g_assert_false(axis_config_get_jobs_concurrency(config, "file-ops", &n));

	text = axis_config_describe(config);
	g_assert_nonnull(strstr(text, "jobs_concurrency=scan:2,probe:4,transcode:0,file-ops.slow:64"));
	g_assert_nonnull(strstr(text, "jobs_grace_period=0"));

	/* An empty list is no list. */
	g_assert_true(axis_config_load_data(config, "[jobs]\nconcurrency =\n", &error));
	g_strfreev(queues);
	queues = axis_config_list_jobs_concurrency(config);
	g_assert_cmpuint(g_strv_length(queues), ==, 0);
}

static void
test_jobs_invalid(void)
{
	static const gchar *const bad_concurrency[] = {
		"scan", "scan:", ":2", "scan:65", "scan:-1", "scan:two", "Scan:1", "1scan:1",
		"scan:1,", ",scan:1", "scan:1,,probe:2", "sc an:1", "scan:1:2", "scan:1,scan:2", NULL
	};
	static const gchar *const bad_grace[] = { "-1", "3601", "30s", "", "3 0", NULL };
	guint i;

	for (i = 0; bad_concurrency[i] != NULL; i++) {
		g_autoptr(AxisConfig) config = axis_config_new();
		g_autoptr(GError) error = NULL;
		g_autofree gchar *data = g_strdup_printf("[jobs]\nconcurrency = %s\n", bad_concurrency[i]);

		if (axis_config_load_data(config, data, &error))
			g_error("concurrency '%s' was accepted", bad_concurrency[i]);
		assert_config_error(error, "jobs.concurrency", NULL);
		g_test_message("%s", error->message);
	}

	for (i = 0; bad_grace[i] != NULL; i++) {
		g_autoptr(AxisConfig) config = axis_config_new();
		g_autoptr(GError) error = NULL;
		g_autofree gchar *data = g_strdup_printf("[jobs]\ngrace_period=%s\n", bad_grace[i]);

		if (axis_config_load_data(config, data, &error))
			g_error("grace period '%s' was accepted", bad_grace[i]);
		assert_config_error(error, "jobs.grace_period", NULL);
	}

	/* Unknown keys are still refused in the new group. */
	{
		g_autoptr(AxisConfig) config = axis_config_new();
		g_autoptr(GError) error = NULL;

		g_assert_false(axis_config_load_data(config, "[jobs]\nconcurency = scan:1\n", &error));
		assert_config_error(error, "jobs", "concurency");
	}
}

static void
test_jobs_environment(void)
{
	g_autoptr(AxisConfig) config = axis_config_new();
	g_autoptr(GError) error = NULL;
	g_auto(GStrv) envp = NULL;
	g_auto(GStrv) bad = NULL;

	g_assert_true(axis_config_load_data(config, "[jobs]\nconcurrency = scan:2\ngrace_period = 10\n",
		&error));
	envp = g_environ_setenv(NULL, "AXIS_JOBS_CONCURRENCY", "probe:3", TRUE);
	envp = g_environ_setenv(envp, "AXIS_JOBS_GRACE_PERIOD", "45", TRUE);
	g_assert_true(axis_config_apply_environment(config, envp, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(axis_config_get_jobs_grace_period(config), ==, 45);
	g_assert_cmpuint(concurrency_of(config, "probe"), ==, 3);
	g_assert_false(axis_config_get_jobs_concurrency(config, "scan", NULL));

	bad = g_environ_setenv(NULL, "AXIS_JOBS_GRACE_PERIOD", "forever", TRUE);
	g_assert_false(axis_config_apply_environment(config, bad, &error));
	assert_config_error(error, "AXIS_JOBS_GRACE_PERIOD", NULL);
	g_clear_error(&error);
	g_strfreev(bad);
	bad = g_environ_setenv(NULL, "AXIS_JOBS_CONCURRENCY", "scan=2", TRUE);
	g_assert_false(axis_config_apply_environment(config, bad, &error));
	assert_config_error(error, "AXIS_JOBS_CONCURRENCY", NULL);
	g_assert_cmpuint(axis_config_get_jobs_grace_period(config), ==, 45);
}

static void
test_proxy_settings(void)
{
	g_autoptr(AxisConfig) config = axis_config_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = NULL;
	g_auto(GStrv) envp = NULL;

	/* Defaults: no proxy is trusted, Secure follows the request. */
	g_assert_cmpstr(axis_config_get_trusted_proxies(config), ==, "");
	g_assert_cmpint(axis_config_get_secure_cookies(config), ==, AXIS_SECURE_COOKIES_AUTO);

	g_assert_true(axis_config_load_data(config,
		"[server]\ntrusted_proxies = 172.16.0.10/12 ,10.0.0.1, ::1\nsecure_cookies = always\n", &error));
	g_assert_no_error(error);
	/* Stored in canonical form. */
	g_assert_cmpstr(axis_config_get_trusted_proxies(config), ==, "172.16.0.0/12, 10.0.0.1, ::1");
	g_assert_cmpint(axis_config_get_secure_cookies(config), ==, AXIS_SECURE_COOKIES_ALWAYS);
	text = axis_config_describe(config);
	g_assert_nonnull(strstr(text, "trusted_proxies=172.16.0.0/12, 10.0.0.1, ::1"));
	g_assert_nonnull(strstr(text, "secure_cookies=always"));

	envp = g_environ_setenv(NULL, "AXIS_TRUSTED_PROXIES", "192.168.5.0/24", TRUE);
	envp = g_environ_setenv(envp, "AXIS_SECURE_COOKIES", "auto", TRUE);
	g_assert_true(axis_config_apply_environment(config, envp, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(axis_config_get_trusted_proxies(config), ==, "192.168.5.0/24");
	g_assert_cmpint(axis_config_get_secure_cookies(config), ==, AXIS_SECURE_COOKIES_AUTO);

	/* An empty list in the file means none. */
	g_assert_true(axis_config_load_data(config, "[server]\ntrusted_proxies =\n", &error));
	g_assert_cmpstr(axis_config_get_trusted_proxies(config), ==, "");
}

/* Spec (daemon-core): Invalid proxy entry */
static void
test_proxy_settings_invalid(void)
{
	static const gchar *const bad_proxies[] = {
		"-proxy.example", "10.0.0.1,,10.0.0.2", "10.0.0.0/33", "10.0.0.0/0", "10.0.0.1:8080",
		"10.0.0.1 10.0.0.2", "proxy.example.", "proxy..example", "traefik/24", "300.1.1.1", NULL
	};
	static const gchar *const bad_secure[] = { "yes", "true", "Always", "", "never", NULL };
	guint i;

	for (i = 0; bad_proxies[i] != NULL; i++) {
		g_autoptr(AxisConfig) config = axis_config_new();
		g_autoptr(GError) error = NULL;
		g_autofree gchar *data = g_strdup_printf("[server]\ntrusted_proxies = %s\n", bad_proxies[i]);

		if (axis_config_load_data(config, data, &error))
			g_error("trusted_proxies '%s' was accepted", bad_proxies[i]);
		assert_config_error(error, "server.trusted_proxies", NULL);
		/* The offending entry is named, unless the list itself is the
		 * problem (an empty entry). */
		if (strstr(bad_proxies[i], ",,") == NULL)
			g_assert_nonnull(strstr(error->message, bad_proxies[i]));
		g_assert_cmpstr(axis_config_get_trusted_proxies(config), ==, "");
	}

	for (i = 0; bad_secure[i] != NULL; i++) {
		g_autoptr(AxisConfig) config = axis_config_new();
		g_autoptr(GError) error = NULL;
		g_autofree gchar *data = g_strdup_printf("[server]\nsecure_cookies = %s\n", bad_secure[i]);

		if (axis_config_load_data(config, data, &error))
			g_error("secure_cookies '%s' was accepted", bad_secure[i]);
		assert_config_error(error, "server.secure_cookies", NULL);
	}

	{
		g_autoptr(AxisConfig) config = axis_config_new();
		g_autoptr(GError) error = NULL;
		g_auto(GStrv) envp = g_environ_setenv(NULL, "AXIS_TRUSTED_PROXIES", "10.0.0.1, no pe", TRUE);

		g_assert_false(axis_config_apply_environment(config, envp, &error));
		assert_config_error(error, "AXIS_TRUSTED_PROXIES", "'no pe'");
		g_clear_error(&error);
		g_strfreev(envp);
		envp = g_environ_setenv(NULL, "AXIS_SECURE_COOKIES", "sometimes", TRUE);
		g_assert_false(axis_config_apply_environment(config, envp, &error));
		assert_config_error(error, "AXIS_SECURE_COOKIES", "'sometimes'");
	}
}

/* ── Secrets from files, the first administrator, the wait ──────── */

#define ADMIN_SECRET "correct-horse-battery-staple"
#define METRICS_SECRET "0123456789abcdef0123456789abcdef"

/* Spec (deployment): Secret file -- every secret has a _FILE form that
 * reads the value from a file, with the newline an editor adds removed. */
static void
test_secret_files(void)
{
	g_autoptr(AxisConfig) config = axis_config_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *url = write_temp("postgres://u:" FILE_SECRET "@h/db\n");
	g_autofree gchar *password = write_temp(ADMIN_SECRET "\n");
	g_autofree gchar *token = write_temp(METRICS_SECRET "\r\n");
	g_auto(GStrv) env = make_env("AXIS_DATABASE_URL_FILE", url, "AXIS_ADMIN_USERNAME", "Admin",
		"AXIS_ADMIN_PASSWORD_FILE", password, "AXIS_METRICS_TOKEN_FILE", token, NULL);
	g_autofree gchar *description = NULL;

	g_assert_true(axis_config_apply_environment(config, env, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(axis_config_get_database_url(config), ==, "postgres://u:" FILE_SECRET "@h/db");
	g_assert_cmpstr(axis_config_get_admin_username(config), ==, "Admin");
	g_assert_cmpstr(axis_config_get_admin_password(config), ==, ADMIN_SECRET);
	g_assert_cmpstr(axis_config_get_metrics_token(config), ==, METRICS_SECRET);
	g_assert_true(axis_config_check_required(config, &error));

	/* The description says only that they are set. */
	description = axis_config_describe(config);
	g_assert_null(strstr(description, FILE_SECRET));
	g_assert_null(strstr(description, ADMIN_SECRET));
	g_assert_null(strstr(description, METRICS_SECRET));
	g_assert_nonnull(strstr(description, "first_admin=(given)"));
	g_assert_nonnull(strstr(description, "metrics=on"));

	/* Used once, the password is gone. */
	axis_config_forget_admin_password(config);
	g_assert_null(axis_config_get_admin_password(config));

	g_unlink(url);
	g_unlink(password);
	g_unlink(token);
}

/* Spec (deployment): both a secret and its _FILE form set is an error
 * naming both, for each secret, and the configuration is left as it was. */
static void
test_secret_and_file_both_set(void)
{
	static const gchar *const variables[] = { "AXIS_DATABASE_URL", "AXIS_ADMIN_PASSWORD", "AXIS_METRICS_TOKEN" };
	g_autofree gchar *path = write_temp(METRICS_SECRET "\n");
	guint i;

	for (i = 0; i < G_N_ELEMENTS(variables); i++) {
		g_autoptr(AxisConfig) config = axis_config_new();
		g_autoptr(GError) error = NULL;
		g_autofree gchar *file_variable = g_strconcat(variables[i], "_FILE", NULL);
		g_autofree gchar *both = g_strconcat(variables[i], " and ", file_variable, NULL);
		g_auto(GStrv) env = make_env(variables[i], METRICS_SECRET "x", file_variable, path,
			"AXIS_LISTEN_PORT", "9100", NULL);

		g_assert_false(axis_config_apply_environment(config, env, &error));
		assert_config_error(error, both, "set only one");
		g_assert_null(strstr(error->message, METRICS_SECRET));
		assert_defaults(config);
		g_assert_null(axis_config_get_admin_password(config));
		g_assert_null(axis_config_get_metrics_token(config));
	}
	g_unlink(path);
}

/* An unreadable or empty secret file names the variable and the path, never
 * what a file holds. */
static void
test_secret_file_unreadable(void)
{
	g_autofree gchar *empty = write_temp("\n\n");
	const gchar *paths[2];
	guint i;

	paths[0] = "/nonexistent/axis/admin-password";
	paths[1] = empty;
	for (i = 0; i < G_N_ELEMENTS(paths); i++) {
		g_autoptr(AxisConfig) config = axis_config_new();
		g_autoptr(GError) error = NULL;
		g_auto(GStrv) env = make_env("AXIS_ADMIN_PASSWORD_FILE", paths[i], NULL);

		g_assert_false(axis_config_apply_environment(config, env, &error));
		assert_config_error(error, "AXIS_ADMIN_PASSWORD_FILE", paths[i]);
		g_assert_null(axis_config_get_admin_password(config));
	}
	g_unlink(empty);
}

/* A username without a password, or the reverse, is refused at start. */
static void
test_admin_needs_both(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisConfig) config = axis_config_new();
	g_auto(GStrv) env = make_env("AXIS_DATABASE_URL", "postgres://u@h/db", "AXIS_ADMIN_USERNAME", "admin", NULL);

	g_assert_true(axis_config_apply_environment(config, env, &error));
	g_assert_false(axis_config_check_required(config, &error));
	assert_config_error(error, "AXIS_ADMIN_PASSWORD_FILE", "the password is not");
	g_clear_error(&error);

	g_clear_pointer(&env, g_strfreev);
	g_clear_pointer(&config, axis_config_free);
	config = axis_config_new();
	env = make_env("AXIS_DATABASE_URL", "postgres://u@h/db", "AXIS_ADMIN_PASSWORD", ADMIN_SECRET, NULL);
	g_assert_true(axis_config_apply_environment(config, env, &error));
	g_assert_false(axis_config_check_required(config, &error));
	assert_config_error(error, "the username is not", NULL);
	g_assert_null(strstr(error->message, ADMIN_SECRET));

	/* From the file too, in [setup]. */
	g_clear_error(&error);
	g_clear_pointer(&config, axis_config_free);
	config = axis_config_new();
	g_assert_true(axis_config_load_data(config,
		"[database]\nurl = postgres://u@h/db\n[setup]\nadmin_username = boss\nadmin_password = " ADMIN_SECRET "\n",
		&error));
	g_assert_true(axis_config_check_required(config, &error));
	g_assert_cmpstr(axis_config_get_admin_username(config), ==, "boss");
	g_assert_cmpstr(axis_config_get_admin_password(config), ==, ADMIN_SECRET);
}

/* The metrics token must be long and printable; the refusal never quotes it. */
static void
test_metrics_token_rules(void)
{
	static const gchar *const bad[] = { "short", "0123456789abcdef 0123456789abcdef", "0123456789abcdef\t0123456789abcdef" };
	guint i;

	for (i = 0; i < G_N_ELEMENTS(bad); i++) {
		g_autoptr(AxisConfig) config = axis_config_new();
		g_autoptr(GError) error = NULL;
		g_auto(GStrv) env = make_env("AXIS_METRICS_TOKEN", bad[i], NULL);

		g_assert_false(axis_config_apply_environment(config, env, &error));
		assert_config_error(error, "AXIS_METRICS_TOKEN", "printable");
		g_assert_null(strstr(error->message, bad[i]));
		g_assert_null(axis_config_get_metrics_token(config));
	}
}

/* database.wait: forever by default, a number of seconds, or 0. */
static void
test_database_wait(void)
{
	static const gchar *const bad[] = { "-1", "86401", "1.5", "soon", "" };
	g_autoptr(AxisConfig) config = axis_config_new();
	g_autoptr(GError) error = NULL;
	guint i;

	g_assert_cmpint(axis_config_get_database_wait(config), ==, AXIS_CONFIG_WAIT_FOREVER);
	g_assert_true(axis_config_load_data(config, "[database]\nwait = 120\n", &error));
	g_assert_cmpint(axis_config_get_database_wait(config), ==, 120);
	g_assert_true(axis_config_load_data(config, "[database]\nwait = 0\n", &error));
	g_assert_cmpint(axis_config_get_database_wait(config), ==, 0);
	g_assert_true(axis_config_load_data(config, "[database]\nwait = forever\n", &error));
	g_assert_cmpint(axis_config_get_database_wait(config), ==, AXIS_CONFIG_WAIT_FOREVER);

	for (i = 0; i < G_N_ELEMENTS(bad); i++) {
		g_autofree gchar *data = g_strdup_printf("[database]\nwait = %s\n", bad[i]);

		g_assert_false(axis_config_load_data(config, data, &error));
		assert_config_error(error, "database.wait", "forever");
		g_clear_error(&error);
	}

	{
		g_auto(GStrv) env = make_env("AXIS_DATABASE_WAIT", "30", NULL);

		g_assert_true(axis_config_apply_environment(config, env, &error));
		g_assert_cmpint(axis_config_get_database_wait(config), ==, 30);
	}
}

/* log.format: text by default, or json. */
static void
test_log_format(void)
{
	g_autoptr(AxisConfig) config = axis_config_new();
	g_autoptr(GError) error = NULL;
	g_auto(GStrv) env = make_env("AXIS_LOG_FORMAT", "json", NULL);
	g_auto(GStrv) bad = make_env("AXIS_LOG_FORMAT", "xml", NULL);

	g_assert_cmpint(axis_config_get_log_format(config), ==, AXIS_LOG_FORMAT_TEXT);
	g_assert_true(axis_config_apply_environment(config, env, &error));
	g_assert_cmpint(axis_config_get_log_format(config), ==, AXIS_LOG_FORMAT_JSON);
	g_assert_false(axis_config_apply_environment(config, bad, &error));
	assert_config_error(error, "AXIS_LOG_FORMAT", "text or json");
}

/* Spec (outbound-binding): The setting.  outbound.bind: none by default, an
 * interface or an address in canonical form, from the file or the
 * environment; a bad value stops the start and says what is expected; and an
 * interface that does not exist is no error here.  outbound.bind_scope:
 * internet by default, or all. */
static void
test_outbound(void)
{
	static const gchar *const bad[] = { "tun 0", "10.8.0", "fe80::1", "127.0.0.1", "eth0/1", "averyveryverylong0" };
	g_autoptr(AxisConfig) config = axis_config_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *description = NULL;
	guint i;

	g_assert_cmpstr(axis_config_get_outbound_bind(config), ==, "");
	g_assert_cmpint(axis_config_get_outbound_scope(config), ==, AXIS_OUTBOUND_SCOPE_INTERNET);
	description = axis_config_describe(config);
	g_assert_nonnull(strstr(description, "outbound_bind=(none) outbound_bind_scope=internet"));
	g_clear_pointer(&description, g_free);

	g_assert_true(axis_config_load_data(config, "[outbound]\nbind = axis-absent0\nbind_scope = all\n", &error));
	g_assert_no_error(error);
	g_assert_cmpstr(axis_config_get_outbound_bind(config), ==, "axis-absent0");
	g_assert_cmpint(axis_config_get_outbound_scope(config), ==, AXIS_OUTBOUND_SCOPE_ALL);
	g_assert_true(axis_config_load_data(config, "[outbound]\nbind = FD00:0::2\n", &error));
	g_assert_cmpstr(axis_config_get_outbound_bind(config), ==, "fd00::2");
	description = axis_config_describe(config);
	g_assert_nonnull(strstr(description, "outbound_bind=fd00::2 outbound_bind_scope=all"));

	for (i = 0; i < G_N_ELEMENTS(bad); i++) {
		g_autofree gchar *data = g_strdup_printf("[outbound]\nbind = %s\n", bad[i]);
		g_auto(GStrv) env = make_env("AXIS_OUTBOUND_BIND", bad[i], NULL);

		g_assert_false(axis_config_load_data(config, data, &error));
		assert_config_error(error, "outbound.bind", "such as tun0 or wg0");
		g_clear_error(&error);
		g_assert_false(axis_config_apply_environment(config, env, &error));
		assert_config_error(error, "AXIS_OUTBOUND_BIND", "such as tun0 or wg0");
		g_clear_error(&error);
		/* Unchanged by a refusal. */
		g_assert_cmpstr(axis_config_get_outbound_bind(config), ==, "fd00::2");
	}
	g_assert_false(axis_config_load_data(config, "[outbound]\nbind_scope = everything\n", &error));
	assert_config_error(error, "outbound.bind_scope", "internet");
	g_clear_error(&error);
	g_assert_false(axis_config_load_data(config, "[outbound]\ninterface = tun0\n", &error));
	assert_config_error(error, "interface", "[outbound]");
	g_clear_error(&error);

	{
		g_auto(GStrv) env = make_env("AXIS_OUTBOUND_BIND", "wg0", "AXIS_OUTBOUND_BIND_SCOPE", "internet", NULL);

		g_assert_true(axis_config_apply_environment(config, env, &error));
		g_assert_cmpstr(axis_config_get_outbound_bind(config), ==, "wg0");
		g_assert_cmpint(axis_config_get_outbound_scope(config), ==, AXIS_OUTBOUND_SCOPE_INTERNET);
	}
	{
		g_auto(GStrv) env = make_env("AXIS_OUTBOUND_BIND_SCOPE", "ALL", NULL);

		g_assert_false(axis_config_apply_environment(config, env, &error));
		assert_config_error(error, "AXIS_OUTBOUND_BIND_SCOPE", "'ALL'");
	}
}

/* What `axisd --healthcheck` reads: no secret and no secret file. */
static void
test_environ_without_secrets(void)
{
	g_auto(GStrv) env = make_env("AXIS_DATABASE_URL", "postgres://u:p@h/db", "AXIS_DATABASE_URL_FILE", "/x",
		"AXIS_ADMIN_PASSWORD", "p", "AXIS_ADMIN_PASSWORD_FILE", "/y", "AXIS_METRICS_TOKEN_FILE", "/z",
		"AXIS_LISTEN_PORT", "9100", "AXIS_ADMIN_USERNAME", "admin", NULL);
	g_auto(GStrv) out = axis_config_environ_without_secrets(env);
	static const gchar *const gone[] = {
		"AXIS_DATABASE_URL", "AXIS_DATABASE_URL_FILE", "AXIS_ADMIN_PASSWORD", "AXIS_ADMIN_PASSWORD_FILE",
		"AXIS_METRICS_TOKEN", "AXIS_METRICS_TOKEN_FILE", NULL
	};
	guint i;

	for (i = 0; gone[i] != NULL; i++)
		g_assert_null(g_environ_getenv(out, gone[i]));
	g_assert_cmpstr(g_environ_getenv(out, "AXIS_LISTEN_PORT"), ==, "9100");
	g_assert_cmpstr(g_environ_getenv(out, "AXIS_ADMIN_USERNAME"), ==, "admin");
	/* The original is untouched. */
	g_assert_nonnull(g_environ_getenv(env, "AXIS_DATABASE_URL"));
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/config/secrets/from-files", test_secret_files);
	g_test_add_func("/config/secrets/both-set", test_secret_and_file_both_set);
	g_test_add_func("/config/secrets/unreadable", test_secret_file_unreadable);
	g_test_add_func("/config/secrets/environ-without", test_environ_without_secrets);
	g_test_add_func("/config/setup/admin-needs-both", test_admin_needs_both);
	g_test_add_func("/config/metrics/token-rules", test_metrics_token_rules);
	g_test_add_func("/config/database/wait", test_database_wait);
	g_test_add_func("/config/log/format", test_log_format);
	g_test_add_func("/config/outbound", test_outbound);
	g_test_add_func("/config/server/proxies", test_proxy_settings);
	g_test_add_func("/config/server/proxies-invalid", test_proxy_settings_invalid);
	g_test_add_func("/config/defaults", test_defaults);
	g_test_add_func("/config/file/values", test_file_values);
	g_test_add_func("/config/file/partial-keeps-defaults", test_partial_file_keeps_defaults);
	g_test_add_func("/config/file/unknown-group", test_unknown_group);
	g_test_add_func("/config/file/unknown-key", test_unknown_key);
	g_test_add_func("/config/file/syntax-error", test_syntax_error);
	g_test_add_func("/config/file/invalid-port", test_invalid_port);
	g_test_add_func("/config/file/port-bounds", test_valid_port_bounds);
	g_test_add_func("/config/file/invalid-level", test_invalid_level);
	g_test_add_func("/config/file/invalid-pool-size", test_invalid_pool_size);
	g_test_add_func("/config/file/pool-size-bounds", test_valid_pool_size_bounds);
	g_test_add_func("/config/required/database-url", test_required_database_url);
	g_test_add_func("/config/file/empty-host", test_empty_host);
	g_test_add_func("/config/file/missing", test_missing_file);
	g_test_add_func("/config/file/failed-load-unchanged", test_failed_load_leaves_config_unchanged);
	g_test_add_func("/config/env/overrides", test_env_overrides);
	g_test_add_func("/config/env/wins-over-file", test_env_wins_over_file);
	g_test_add_func("/config/env/invalid-value", test_env_invalid_value);
	g_test_add_func("/config/env/empty-ignored", test_env_empty_ignored);
	g_test_add_func("/config/server/portal-listener", test_portal_listener);
	g_test_add_func("/config/env/url-file/read", test_url_file_read);
	g_test_add_func("/config/env/url-file/wins-over-config-file", test_url_file_wins_over_config_file);
	g_test_add_func("/config/env/url-file/both-set", test_url_file_and_url_both_set);
	g_test_add_func("/config/env/url-file/unreadable", test_url_file_unreadable);
	g_test_add_func("/config/env/url-file/described", test_url_file_described_without_password);
	g_test_add_func("/config/redact/url-form", test_redact_url_form);
	g_test_add_func("/config/redact/query-form", test_redact_query_form);
	g_test_add_func("/config/redact/keyword-form", test_redact_keyword_form);
	g_test_add_func("/config/redact/garbage", test_redact_garbage);
	g_test_add_func("/config/redact/null-and-empty", test_redact_null_and_empty);
	g_test_add_func("/config/describe/no-password", test_describe_never_contains_password);
	g_test_add_func("/config/describe/no-database", test_describe_without_database);
	g_test_add_func("/config/describe/pool-size-used", test_describe_pool_size_used);
	g_test_add_func("/config/jobs/defaults", test_jobs_defaults);
	g_test_add_func("/config/jobs/file-values", test_jobs_file_values);
	g_test_add_func("/config/jobs/invalid", test_jobs_invalid);
	g_test_add_func("/config/jobs/environment", test_jobs_environment);

	return g_test_run();
}
