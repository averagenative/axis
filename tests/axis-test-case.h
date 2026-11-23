/*
 * axis-test-case.h - tests whose body is not entered without a database
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A test with a fixture that needs PostgreSQL is skipped when
 * AXIS_SKIP_DB_TESTS=1 (the image build runs the tests that way): its
 * fixture is then empty.  A test function that touches the fixture in the
 * initialisers of its own variables has done so before any check it makes
 * in its body can run, and crashes instead of skipping.  Registering the
 * test through the wrapper below keeps the whole function from being
 * entered, so no test has to remember.
 *
 * Use, once, after fixture_setup() and fixture_teardown() are defined:
 *
 *   AXIS_TEST_DB_CASES(Fixture, fx->t != NULL)
 *
 * (the second argument says, in terms of `fx`, whether the fixture got its
 * database), and then register with
 *
 *   AXIS_TEST_ADD_DB_CASE("/path", Fixture, data, test_function);
 */

#ifndef AXIS_TEST_CASE_H
#define AXIS_TEST_CASE_H

#include <glib.h>

#define AXIS_TEST_DB_CASES(FixtureType, has_database) \
	typedef struct \
	{ \
		void          (*func)(FixtureType *fx, gconstpointer data); \
		gconstpointer   data; \
	} AxisTestDbCase; \
	\
	static void \
	axis_test_db_case_setup(FixtureType *fx, gconstpointer data) \
	{ \
		const AxisTestDbCase *test_case = data; \
	\
		fixture_setup(fx, test_case->data); \
	} \
	\
	static void \
	axis_test_db_case_run(FixtureType *fx, gconstpointer data) \
	{ \
		const AxisTestDbCase *test_case = data; \
	\
		/* Skipped: fixture_setup() said so already. */ \
		if (!(has_database)) \
			return; \
		test_case->func(fx, test_case->data); \
	} \
	\
	static void \
	axis_test_db_case_teardown(FixtureType *fx, gconstpointer data) \
	{ \
		const AxisTestDbCase *test_case = data; \
	\
		fixture_teardown(fx, test_case->data); \
	}

#define AXIS_TEST_ADD_DB_CASE(path, FixtureType, case_data, test_func) \
	do { \
		static AxisTestDbCase axis_test_db_case; \
	\
		axis_test_db_case.func = (test_func); \
		axis_test_db_case.data = (case_data); \
		g_test_add((path), FixtureType, &axis_test_db_case, axis_test_db_case_setup, \
			axis_test_db_case_run, axis_test_db_case_teardown); \
	} while (0)

#endif /* AXIS_TEST_CASE_H */
