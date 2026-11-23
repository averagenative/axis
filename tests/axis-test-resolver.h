/*
 * axis-test-resolver.h - a GResolver that answers from a table, for tests
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * No test looks a name up through the system's resolver: it installs this as
 * the default (g_resolver_set_default()) and says what each name resolves
 * to.  An answer arrives on the next iteration of the main context, as a
 * real one would, or never until released (hold).
 */

#ifndef AXIS_TEST_RESOLVER_H
#define AXIS_TEST_RESOLVER_H

#include <gio/gio.h>

G_BEGIN_DECLS

/* A new fake; make it the default with g_resolver_set_default(). */
GResolver *
axis_test_resolver_new(void);

/* @name resolves to @addresses (a NULL-terminated array of address texts);
 * %NULL makes the lookup fail with "no such host". */
void
axis_test_resolver_set(
	GResolver          *resolver,
	const gchar        *name,
	const gchar *const *addresses
);

/* While held, a lookup is not answered until axis_test_resolver_release(). */
void
axis_test_resolver_hold(
	GResolver *resolver,
	gboolean   hold
);

/* Answers every lookup held so far (at the next main loop iteration). */
void
axis_test_resolver_release(GResolver *resolver);

/* How many lookups of @name were asked for. */
guint
axis_test_resolver_count(
	GResolver   *resolver,
	const gchar *name
);

/* Runs the default main context until nothing is ready. */
void
axis_test_resolver_drain(void);

G_END_DECLS

#endif /* AXIS_TEST_RESOLVER_H */
