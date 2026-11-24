/*
 * axis-secret.c - random tokens, what is derived from them, and the care a
 * secret is compared and freed with
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "web/axis-secret.h"

#include <sodium.h>
#include <string.h>

gboolean
axis_secret_init(void)
{
	return sodium_init() >= 0;
}

void
axis_secret_free(gchar *secret)
{
	if (secret == NULL)
		return;

	sodium_memzero(secret, strlen(secret));
	g_free(secret);
}

static gchar *
to_base64url(
	const guint8 *bytes,
	gsize         len
){
	gsize size = sodium_base64_ENCODED_LEN(len, sodium_base64_VARIANT_URLSAFE_NO_PADDING);
	gchar *out = g_malloc(size);

	sodium_bin2base64(out, size, bytes, len, sodium_base64_VARIANT_URLSAFE_NO_PADDING);
	return out;
}

gchar *
axis_random_token(void)
{
	guint8 bytes[32];
	gchar *token;

	randombytes_buf(bytes, sizeof(bytes));
	token = to_base64url(bytes, sizeof(bytes));
	sodium_memzero(bytes, sizeof(bytes));

	return token;
}

gchar *
axis_random_public_id(void)
{
	guint8 bytes[16];

	randombytes_buf(bytes, sizeof(bytes));
	return to_base64url(bytes, sizeof(bytes));
}

gchar *
axis_random_setup_token(void)
{
	guint8 bytes[16];
	gchar *token = g_malloc(sizeof(bytes) * 2 + 1);

	randombytes_buf(bytes, sizeof(bytes));
	sodium_bin2hex(token, sizeof(bytes) * 2 + 1, bytes, sizeof(bytes));
	sodium_memzero(bytes, sizeof(bytes));

	return token;
}

gchar *
axis_token_derive(
	const gchar *token,
	const gchar *purpose
){
	guint8 out[32];
	gsize key_len;
	gchar *derived;

	g_return_val_if_fail(token != NULL, NULL);
	g_return_val_if_fail(purpose != NULL, NULL);

	key_len = strlen(token);
	g_return_val_if_fail(key_len >= crypto_generichash_KEYBYTES_MIN, NULL);
	g_return_val_if_fail(key_len <= crypto_generichash_KEYBYTES_MAX, NULL);

	crypto_generichash(out, sizeof(out), (const guint8 *)purpose, strlen(purpose),
		(const guint8 *)token, key_len);
	derived = to_base64url(out, sizeof(out));
	sodium_memzero(out, sizeof(out));

	return derived;
}

gchar *
axis_token_hash(const gchar *token)
{
	guint8 digest[crypto_hash_sha256_BYTES];
	gchar *hex = g_malloc(sizeof(digest) * 2 + 1);

	g_return_val_if_fail(token != NULL, NULL);

	crypto_hash_sha256(digest, (const guint8 *)token, strlen(token));
	sodium_bin2hex(hex, sizeof(digest) * 2 + 1, digest, sizeof(digest));

	return hex;
}

gboolean
axis_secret_equal(
	const gchar *a,
	const gchar *b
){
	gsize len;

	if (a == NULL || b == NULL)
		return FALSE;

	len = strlen(a);
	if (strlen(b) != len)
		return FALSE;
	if (len == 0)
		return TRUE;

	return sodium_memcmp(a, b, len) == 0;
}
