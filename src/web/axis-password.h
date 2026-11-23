/*
 * axis-password.h - password hashing (argon2id, libsodium) and secrets
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Hashing and verifying are deliberately slow and use a lot of memory
 * (libsodium's "moderate" argon2id limits: three passes over 256 MiB), so
 * they block the calling thread for a long time: never call them on the
 * main loop's thread.  Everything here is thread-safe.
 *
 * The random tokens of the web interface come from here too, from
 * libsodium's generator.
 */

#ifndef AXIS_PASSWORD_H
#define AXIS_PASSWORD_H

#include <glib.h>

G_BEGIN_DECLS

/* The text length of a 32-byte random token (URL-safe base64, no padding). */
#define AXIS_TOKEN_LEN 43

/* The text length of a 16-byte random identifier (same encoding). */
#define AXIS_PUBLIC_ID_LEN 22

/* The text length of the setup token (16 bytes as hexadecimal). */
#define AXIS_SETUP_TOKEN_LEN 32

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

/**
 * axis_secret_free:
 * @secret: (nullable): a string holding a password or token
 *
 * Overwrites @secret with zeros, then frees it.
 */
void
axis_secret_free(gchar *secret);

/**
 * axis_random_token:
 *
 * Returns: (transfer full): 32 random bytes as %AXIS_TOKEN_LEN URL-safe
 *   base64 characters
 */
gchar *
axis_random_token(void);

/**
 * axis_random_public_id:
 *
 * Returns: (transfer full): 16 random bytes as %AXIS_PUBLIC_ID_LEN URL-safe
 *   base64 characters
 */
gchar *
axis_random_public_id(void);

/**
 * axis_random_setup_token:
 *
 * Returns: (transfer full): 16 random bytes as %AXIS_SETUP_TOKEN_LEN
 *   lower-case hexadecimal digits
 */
gchar *
axis_random_setup_token(void);

/**
 * axis_token_derive:
 * @token: a secret token
 * @purpose: what the derived token is for, such as "csrf"
 *
 * A keyed BLAKE2b of @purpose under @token: the same inputs always give
 * the same result, and it reveals nothing about @token.
 *
 * Returns: (transfer full): the derived token, %AXIS_TOKEN_LEN URL-safe
 *   base64 characters
 */
gchar *
axis_token_derive(
	const gchar *token,
	const gchar *purpose
);

/**
 * axis_token_hash:
 * @token: a secret token
 *
 * Returns: (transfer full): the SHA-256 of @token as 64 lower-case
 *   hexadecimal digits: what the database stores instead of the token
 */
gchar *
axis_token_hash(const gchar *token);

/**
 * axis_secret_equal:
 * @a: (nullable): a secret
 * @b: (nullable): another
 *
 * Compares in time that depends only on the lengths, which are not
 * secret here.
 *
 * Returns: %TRUE if both are non-%NULL and equal
 */
gboolean
axis_secret_equal(
	const gchar *a,
	const gchar *b
);

G_END_DECLS

#endif /* AXIS_PASSWORD_H */
