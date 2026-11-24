/*
 * axis-password.h - password hashing (argon2id, libsodium) and secrets
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Hashing and verifying are deliberately slow and use a lot of memory
 * (libsodium's "moderate" argon2id limits: three passes over 256 MiB), so
 * they block the calling thread for a long time: never call them on the
 * main loop's thread.  Everything here is thread-safe.
 *
 * The random tokens of the web interface, and the other secrets' helpers,
 * are in axis-secret.h, which this header includes.
 */

#ifndef AXIS_PASSWORD_H
#define AXIS_PASSWORD_H

#include <glib.h>

#include "web/axis-secret.h"

G_BEGIN_DECLS

/**
 * axis_password_init:
 *
 * Initialises libsodium.  Call once before anything else here; safe to
 * call again.
 *
 * Returns: %FALSE if libsodium cannot be used (no randomness source)
 */
gboolean
axis_password_init(void);

/**
 * axis_password_hash:
 * @password: the password (its length is not checked here)
 * @error: (nullable): return location for an error
 *
 * Fails with %AXIS_ERROR_FAILED when the memory for the hash cannot be
 * had.  The message never contains the password.
 *
 * Returns: (transfer full) (nullable): the argon2id hash string
 *   ("$argon2id$v=19$..."), or %NULL on error
 */
gchar *
axis_password_hash(
	const gchar  *password,
	GError      **error
);

/**
 * axis_password_verify:
 * @stored: (nullable): a hash from axis_password_hash()
 * @password: the password to check
 *
 * Returns: %TRUE if @password is the one @stored was made from.  A
 *   %NULL or malformed @stored never matches, and still takes the time of
 *   a full verification.
 */
gboolean
axis_password_verify(
	const gchar *stored,
	const gchar *password
);

/* How many times axis_password_verify() was called since the process began
 * (each call is one password hash): for the tests that count them.  Any
 * thread. */
guint
axis_password_verify_count(void);

/**
 * axis_password_prepare:
 *
 * Makes now the dummy hash that axis_password_verify() checks against when
 * there is no stored hash.  It is otherwise made on first need, which
 * would make that one failed login measurably slower than the others.
 * Blocks as long as a hash takes.
 */
void
axis_password_prepare(void);

/**
 * axis_password_needs_rehash:
 *
 * Returns: %TRUE if @stored was made with other limits than the ones
 *   axis_password_hash() uses now
 */
gboolean
axis_password_needs_rehash(const gchar *stored);

/**
 * axis_password_use_test_cost:
 *
 * For tests only: from now on hash with libsodium's minimum limits, so
 * that a test can hash hundreds of times.  Never called by the daemon.
 */
void
axis_password_use_test_cost(void);

G_END_DECLS

#endif /* AXIS_PASSWORD_H */
