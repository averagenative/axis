/*
 * axis-secret.h - random tokens, what is derived from them, and the care a
 * secret is compared and freed with
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Split from axis-password.h (add-request-portal, slice P3) so that a program
 * that never checks a password (axis-portal) can have these without the
 * password hashing: this file needs libsodium and GLib, and nothing else of
 * Axis.  Everything here is thread-safe.
 */

#ifndef AXIS_SECRET_H
#define AXIS_SECRET_H

#include <glib.h>

G_BEGIN_DECLS

/* The text length of a 32-byte random token (URL-safe base64, no padding). */
#define AXIS_TOKEN_LEN 43

/* The text length of a 16-byte random identifier (same encoding). */
#define AXIS_PUBLIC_ID_LEN 22

/* The text length of the setup token (16 bytes as hexadecimal). */
#define AXIS_SETUP_TOKEN_LEN 32

/**
 * axis_secret_init:
 *
 * Initialises libsodium.  Call once before anything else here; safe to
 * call again.
 *
 * Returns: %FALSE if libsodium cannot be used (no randomness source)
 */
gboolean
axis_secret_init(void);

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

#endif /* AXIS_SECRET_H */
