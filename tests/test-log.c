/*
 * test-log.c - level filtering and line format
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <glib.h>
#include <json-glib/json-glib.h>
#include <string.h>

#include "core/axis-log.h"

static void
test_level_names(void)
{
	AxisLogLevel level = AXIS_LOG_LEVEL_ERROR;

	g_assert_true(axis_log_level_from_string("debug", &level));
	g_assert_cmpint(level, ==, AXIS_LOG_LEVEL_DEBUG);
	g_assert_true(axis_log_level_from_string("info", &level));
	g_assert_cmpint(level, ==, AXIS_LOG_LEVEL_INFO);
	g_assert_true(axis_log_level_from_string("warning", &level));
	g_assert_cmpint(level, ==, AXIS_LOG_LEVEL_WARNING);
	g_assert_true(axis_log_level_from_string("error", &level));
	g_assert_cmpint(level, ==, AXIS_LOG_LEVEL_ERROR);

	level = AXIS_LOG_LEVEL_WARNING;
	g_assert_false(axis_log_level_from_string("INFO", &level));
	g_assert_false(axis_log_level_from_string("", &level));
	g_assert_false(axis_log_level_from_string(NULL, &level));
	g_assert_cmpint(level, ==, AXIS_LOG_LEVEL_WARNING);

	g_assert_cmpstr(axis_log_level_to_string(AXIS_LOG_LEVEL_WARNING), ==, "warning");
}

static void
test_from_glib(void)
{
	g_assert_cmpint(axis_log_level_from_glib(G_LOG_LEVEL_DEBUG), ==, AXIS_LOG_LEVEL_DEBUG);
	g_assert_cmpint(axis_log_level_from_glib(G_LOG_LEVEL_INFO), ==, AXIS_LOG_LEVEL_INFO);
	g_assert_cmpint(axis_log_level_from_glib(G_LOG_LEVEL_MESSAGE), ==, AXIS_LOG_LEVEL_INFO);
	g_assert_cmpint(axis_log_level_from_glib(G_LOG_LEVEL_WARNING), ==, AXIS_LOG_LEVEL_WARNING);
	g_assert_cmpint(axis_log_level_from_glib(G_LOG_LEVEL_CRITICAL), ==, AXIS_LOG_LEVEL_ERROR);
	g_assert_cmpint(axis_log_level_from_glib(G_LOG_LEVEL_ERROR), ==, AXIS_LOG_LEVEL_ERROR);
	/* fatal and recursion flags do not change the level */
	g_assert_cmpint(
		axis_log_level_from_glib(G_LOG_LEVEL_WARNING | G_LOG_FLAG_FATAL | G_LOG_FLAG_RECURSION),
		==, AXIS_LOG_LEVEL_WARNING);
}

static void
test_filter_table(void)
{
	static const GLogLevelFlags glib_levels[] = {
		G_LOG_LEVEL_DEBUG, G_LOG_LEVEL_INFO, G_LOG_LEVEL_MESSAGE,
		G_LOG_LEVEL_WARNING, G_LOG_LEVEL_CRITICAL, G_LOG_LEVEL_ERROR
	};
	/* [threshold][message level], rows in the same order as glib_levels */
	static const gboolean expect[4][6] = {
		/* debug   */ { 1, 1, 1, 1, 1, 1 },
		/* info    */ { 0, 1, 1, 1, 1, 1 },
		/* warning */ { 0, 0, 0, 1, 1, 1 },
		/* error   */ { 0, 0, 0, 0, 1, 1 }
	};
	guint t;
	guint m;

	for (t = 0; t < 4; t++) {
		for (m = 0; m < G_N_ELEMENTS(glib_levels); m++) {
			g_assert_cmpint(
				axis_log_should_write(glib_levels[m], (AxisLogLevel)t),
				==, expect[t][m]);
		}
	}
}

static void
test_set_get_level(void)
{
	axis_log_set_level(AXIS_LOG_LEVEL_WARNING);
	g_assert_cmpint(axis_log_get_level(), ==, AXIS_LOG_LEVEL_WARNING);
	axis_log_set_level(AXIS_LOG_LEVEL_DEBUG);
	g_assert_cmpint(axis_log_get_level(), ==, AXIS_LOG_LEVEL_DEBUG);
}

static void
test_format(void)
{
	g_autofree gchar *line = axis_log_format(G_LOG_LEVEL_WARNING, "dom", "hello");
	GRegex *re;

	re = g_regex_new("^\\d{4}-\\d{2}-\\d{2}T\\d{2}:\\d{2}:\\d{2}Z WARNING dom: hello$",
		0, 0, NULL);
	g_assert_true(g_regex_match(re, line, 0, NULL));
	g_regex_unref(re);
}

static void
test_format_flattens_control_characters(void)
{
	g_autofree gchar *line = axis_log_format(G_LOG_LEVEL_INFO, "dom",
		"first\nsecond\r\nthird\ttab\x1b[31m");

	g_assert_null(strchr(line, '\n'));
	g_assert_null(strchr(line, '\r'));
	g_assert_null(strchr(line, '\t'));
	g_assert_null(strchr(line, '\x1b'));
	g_assert_nonnull(strstr(line, "INFO dom: first second  third tab [31m"));
}

static void
test_format_keeps_utf8(void)
{
	g_autofree gchar *line = axis_log_format(G_LOG_LEVEL_INFO, "dom", "caf\xc3\xa9 \xe2\x9c\x93");

	g_assert_true(g_str_has_suffix(line, "caf\xc3\xa9 \xe2\x9c\x93"));
	g_assert_true(g_utf8_validate(line, -1, NULL));
}

static void
test_format_null_arguments(void)
{
	g_autofree gchar *line = axis_log_format(G_LOG_LEVEL_ERROR, NULL, NULL);

	g_assert_nonnull(strstr(line, " ERROR "));
}

/* Runs in a child process: installs the real writer. */
static void
child_writer_at_info(void)
{
	/* g_test_init() makes warnings fatal; this child logs them on purpose. */
	g_log_set_always_fatal(G_LOG_FATAL_MASK);
	axis_log_init(AXIS_LOG_LEVEL_INFO);
	g_debug("hidden-debug-message");
	g_info("shown-info-message");
	g_warning("shown-warning-message");
}

static void
child_writer_at_debug_then_error(void)
{
	g_log_set_always_fatal(G_LOG_FATAL_MASK);
	axis_log_init(AXIS_LOG_LEVEL_DEBUG);
	g_debug("shown-debug-message");
	axis_log_set_level(AXIS_LOG_LEVEL_ERROR);
	g_message("hidden-info-message");
	g_critical("shown-critical-message");
	g_critical("multi\nline");
}

static void
test_writer_hides_debug_at_info(void)
{
	if (g_test_subprocess()) {
		child_writer_at_info();
		return;
	}

	g_test_trap_subprocess(NULL, 0, G_TEST_SUBPROCESS_DEFAULT);
	g_test_trap_assert_passed();
	g_test_trap_assert_stderr("*shown-info-message*");
	g_test_trap_assert_stderr("*WARNING axis: shown-warning-message*");
	g_test_trap_assert_stderr_unmatched("*hidden-debug-message*");
}

static void
test_writer_level_change(void)
{
	if (g_test_subprocess()) {
		child_writer_at_debug_then_error();
		return;
	}

	g_test_trap_subprocess(NULL, 0, G_TEST_SUBPROCESS_DEFAULT);
	g_test_trap_assert_passed();
	g_test_trap_assert_stderr("*DEBUG axis: shown-debug-message*");
	g_test_trap_assert_stderr("*ERROR axis: shown-critical-message*");
	g_test_trap_assert_stderr("*multi line*");
	g_test_trap_assert_stderr_unmatched("*hidden-info-message*");
}

/* Parses @line as JSON and checks it is exactly the four members, with the
 * message given. */
static void
assert_json_line(
	const gchar *line,
	const gchar *level,
	const gchar *domain,
	const gchar *message
){
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(GError) error = NULL;
	JsonObject *object;

	g_assert_null(strchr(line, '\n'));
	g_assert_true(g_utf8_validate(line, -1, NULL));
	json_parser_load_from_data(parser, line, -1, &error);
	g_assert_no_error(error);
	g_assert_true(JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)));
	object = json_node_get_object(json_parser_get_root(parser));
	g_assert_cmpuint(json_object_get_size(object), ==, 4);
	g_assert_cmpuint(strlen(json_object_get_string_member(object, "time")), ==, 20);
	g_assert_cmpstr(json_object_get_string_member(object, "level"), ==, level);
	g_assert_cmpstr(json_object_get_string_member(object, "domain"), ==, domain);
	g_assert_cmpstr(json_object_get_string_member(object, "message"), ==, message);
}

/* Spec (deployment): JSON logs -- one object per line, valid JSON whatever
 * the message holds, and the same text as the text format. */
static void
test_format_json(void)
{
	g_autofree gchar *plain = axis_log_format_json(G_LOG_LEVEL_WARNING, "axis", "disk is full");
	g_autofree gchar *tricky = axis_log_format_json(G_LOG_LEVEL_INFO, "axis",
		"quote \" back\\slash\nnew line\ttab \x01 bell\x7f");
	g_autofree gchar *invalid = axis_log_format_json(G_LOG_LEVEL_DEBUG, NULL, "bad \xff\xfe bytes");
	g_autofree gchar *empty = axis_log_format_json(G_LOG_LEVEL_CRITICAL, NULL, NULL);

	assert_json_line(plain, "warning", "axis", "disk is full");
	assert_json_line(tricky, "info", "axis", "quote \" back\\slash\nnew line\ttab \x01 bell\x7f");
	assert_json_line(invalid, "debug", "-", "bad \xef\xbf\xbd\xef\xbf\xbd bytes");
	assert_json_line(empty, "error", "-", "");
}

static void
child_writer_json(void)
{
	g_log_set_always_fatal(G_LOG_FATAL_MASK);
	axis_log_init(AXIS_LOG_LEVEL_INFO);
	axis_log_set_format(AXIS_LOG_FORMAT_JSON);
	g_debug("hidden-debug-message");
	g_message("shown \"json\"\nmessage");
}

static void
test_writer_json(void)
{
	if (g_test_subprocess()) {
		child_writer_json();
		return;
	}

	g_test_trap_subprocess(NULL, 0, G_TEST_SUBPROCESS_DEFAULT);
	g_test_trap_assert_passed();
	g_test_trap_assert_stderr("*{\"time\":\"*\",\"level\":\"info\",\"domain\":\"axis\","
		"\"message\":\"shown \\\"json\\\"\\nmessage\"}\n*");
	g_test_trap_assert_stderr_unmatched("*hidden-debug-message*");
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/log/format/json", test_format_json);
	g_test_add_func("/log/writer/json", test_writer_json);
	g_test_add_func("/log/level-names", test_level_names);
	g_test_add_func("/log/from-glib", test_from_glib);
	g_test_add_func("/log/filter-table", test_filter_table);
	g_test_add_func("/log/set-get-level", test_set_get_level);
	g_test_add_func("/log/format/line", test_format);
	g_test_add_func("/log/format/flattens-control-characters", test_format_flattens_control_characters);
	g_test_add_func("/log/format/keeps-utf8", test_format_keeps_utf8);
	g_test_add_func("/log/format/null-arguments", test_format_null_arguments);
	g_test_add_func("/log/writer/hides-debug-at-info", test_writer_hides_debug_at_info);
	g_test_add_func("/log/writer/level-change", test_writer_level_change);

	return g_test_run();
}
