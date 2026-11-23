/*
 * test-size.c - sizes in words (core/axis-size.h), the one way the interface
 * writes them
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Binary units; one decimal at most; a whole number without a fraction; a
 * no-break space between the number and its unit; and two sizes that differ
 * can be written to read differently.
 */

#include <glib.h>
#include <string.h>

#include "core/axis-size.h"

#define NBSP "\302\240"
#define KIB G_GUINT64_CONSTANT(1024)
#define MIB (KIB * 1024)
#define GIB (MIB * 1024)

static void
assert_size(
	guint64      bytes,
	const gchar *want
){
	g_autofree gchar *text = axis_size_format(bytes);

	g_assert_cmpstr(text, ==, want);
}

static void
test_units(void)
{
	/* "bytes" follows a plain space, as g_format_size() always set it. */
	assert_size(0, "0 bytes");
	assert_size(1, "1 byte");
	assert_size(512, "512 bytes");
	assert_size(1023, "1023 bytes");
	assert_size(1024, "1" NBSP "KiB");
	assert_size(1536, "1.5" NBSP "KiB");
	assert_size(MIB, "1" NBSP "MiB");
	assert_size(850 * MIB, "850" NBSP "MiB");
	assert_size(GIB, "1" NBSP "GiB");
	assert_size(GIB * 5 / 2, "2.5" NBSP "GiB");
	assert_size(1024 * GIB, "1" NBSP "TiB");
}

/* Spec: a size that is exactly 25 GiB is written "25 GiB". */
static void
test_whole_numbers_have_no_fraction(void)
{
	assert_size(25 * GIB, "25" NBSP "GiB");
	/* Within a tenth of a whole number it is that whole number. */
	assert_size(25 * GIB + 10 * MIB, "25" NBSP "GiB");
	assert_size(25 * GIB - 10 * MIB, "25" NBSP "GiB");
	assert_size(24 * GIB + MIB * 1024 * 9 / 10, "24.9" NBSP "GiB");
	assert_size(2 * GIB + GIB * 4 / 10, "2.4" NBSP "GiB");
	assert_size(100 * MIB, "100" NBSP "MiB");
	assert_size(23842 * MIB, "23.3" NBSP "GiB");
}

/* The unit is the one the rounded number belongs to: just under a
 * mebibyte is a mebibyte, not "1024 KiB". */
static void
test_rounding_up_to_the_next_unit(void)
{
	assert_size(MIB - 1, "1" NBSP "MiB");
	assert_size(GIB - 1, "1" NBSP "GiB");
	assert_size(1023 * MIB + MIB * 96 / 100, "1" NBSP "GiB");
	assert_size(1023 * MIB + MIB / 2, "1023.5" NBSP "MiB");
	assert_size(1022 * MIB, "1022" NBSP "MiB");
}

/* No size, over a wide range, ends in ".0" or uses a decimal unit. */
static void
test_never_a_trailing_zero(void)
{
	guint64 bytes;
	guint i;

	for (i = 0, bytes = 1; i < 6000; i++, bytes = bytes * 3 / 2 + 7 * i) {
		g_autofree gchar *text = axis_size_format(bytes);

		g_assert_null(strstr(text, ".0" NBSP));
		if (bytes >= 1024) {
			g_assert_nonnull(strstr(text, NBSP));
			g_assert_null(strstr(text, " "));
		}
		g_assert_false(g_str_has_suffix(text, " GB") || g_str_has_suffix(text, " MB") || g_str_has_suffix(text, "KB"));
		g_assert_true(g_utf8_validate(text, -1, NULL));
	}
}

/* Two sizes written in one unit with enough decimals to differ. */
static void
test_pairs_in_one_unit(void)
{
	guint64 a = 2 * GIB + GIB * 43 / 100;
	guint64 b = 2 * GIB + GIB * 42 / 100;
	g_autofree gchar *x = axis_size_format_as(a, axis_size_unit_of(a), 2);
	g_autofree gchar *y = axis_size_format_as(b, axis_size_unit_of(a), 2);
	g_autofree gchar *whole = axis_size_format_as(25 * GIB, 3, 2);
	g_autofree gchar *number = axis_size_number_as(25 * GIB + GIB / 2, 3, 4);
	g_autofree gchar *old = axis_size_number_as(5722 * MIB, 3, 4);
	g_autofree gchar *none = axis_size_number_as(0, 3, 4);

	g_assert_cmpuint(axis_size_unit_of(a), ==, 3);
	g_assert_cmpstr(x, ==, "2.43" NBSP "GiB");
	g_assert_cmpstr(y, ==, "2.42" NBSP "GiB");
	g_assert_cmpstr(whole, ==, "25" NBSP "GiB");
	g_assert_cmpstr(number, ==, "25.5");
	/* A limit in mebibytes, in GiB to four decimals, is read back whole. */
	g_assert_cmpstr(old, ==, "5.5879");
	g_assert_cmpstr(none, ==, "0");
}

int
main(
	int    argc,
	char **argv
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/size/units", test_units);
	g_test_add_func("/size/whole-numbers-have-no-fraction", test_whole_numbers_have_no_fraction);
	g_test_add_func("/size/rounds-up-to-the-next-unit", test_rounding_up_to_the_next_unit);
	g_test_add_func("/size/never-a-trailing-zero", test_never_a_trailing_zero);
	g_test_add_func("/size/pairs-in-one-unit", test_pairs_in_one_unit);

	return g_test_run();
}
