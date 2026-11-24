/*
 * axis-password.c - password hashing (argon2id, libsodium) and secrets
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "web/axis-password.h"
#include "core/axis-error.h"

#include <sodium.h>
#include <string.h>

/* The limits axis_password_hash() uses; the test cost lowers them. */
static gsize opslimit = crypto_pwhash_argon2id_OPSLIMIT_MODERATE;
static gsize memlimit = crypto_pwhash_argon2id_MEMLIMIT_MODERATE;
static GMutex cost_lock;

/* A hash of a random password nobody knows, made with the current limits,
 * verified against when there is no account so that the answer takes as
 * long as with one. */
static gchar *dummy_hash;
static gsize dummy_opslimit;
static gsize dummy_memlimit;

gboolean
axis_password_init(void)
{
	return sodium_init() >= 0;
}

static void
get_cost(
	gsize *ops,
	gsize *mem
){
	g_mutex_lock(&cost_lock);
	*ops = opslimit;
	*mem = memlimit;
	g_mutex_unlock(&cost_lock);
}

void
axis_password_use_test_cost(void)
{
	g_mutex_lock(&cost_lock);
	opslimit = crypto_pwhash_argon2id_OPSLIMIT_MIN;
	memlimit = crypto_pwhash_argon2id_MEMLIMIT_MIN;
	g_mutex_unlock(&cost_lock);
}

gchar *
axis_password_hash(
	const gchar  *password,
	GError      **error
){
	gchar out[crypto_pwhash_argon2id_STRBYTES];
	gsize ops;
	gsize mem;

	g_return_val_if_fail(password != NULL, NULL);

	get_cost(&ops, &mem);
	if (crypto_pwhash_argon2id_str(out, password, strlen(password),
	                               (unsigned long long)ops, mem) != 0) {
		g_set_error_literal(error, AXIS_ERROR, AXIS_ERROR_FAILED,
			"not enough memory to hash a password");
		return NULL;
	}

	return g_strdup(out);
}

/* The dummy hash for the current limits, made on first use. */
static gchar *
get_dummy_hash(void)
{
	gsize ops;
	gsize mem;
	gchar *hash = NULL;

	get_cost(&ops, &mem);

	g_mutex_lock(&cost_lock);
	if (dummy_hash != NULL && dummy_opslimit == ops && dummy_memlimit == mem)
		hash = g_strdup(dummy_hash);
	g_mutex_unlock(&cost_lock);
	if (hash != NULL)
		return hash;

	{
		gchar *secret = axis_random_token();

		hash = axis_password_hash(secret, NULL);
		axis_secret_free(secret);
	}
	if (hash == NULL)
		return NULL;

	g_mutex_lock(&cost_lock);
	g_free(dummy_hash);
	dummy_hash = g_strdup(hash);
	dummy_opslimit = ops;
	dummy_memlimit = mem;
	g_mutex_unlock(&cost_lock);

	return hash;
}

void
axis_password_prepare(void)
{
	g_free(get_dummy_hash());
}

/* How many verifications were begun (atomic): for tests. */
static gint verifications;

guint
axis_password_verify_count(void)
{
	return (guint)g_atomic_int_get(&verifications);
}

gboolean
axis_password_verify(
	const gchar *stored,
	const gchar *password
){
	g_autofree gchar *dummy = NULL;
	gboolean usable;
	gint result;

	g_return_val_if_fail(password != NULL, FALSE);

	g_atomic_int_inc(&verifications);
	usable = stored != NULL && g_str_has_prefix(stored, "$argon2id$") &&
	         strlen(stored) < crypto_pwhash_argon2id_STRBYTES;
	if (!usable) {
		/* Same work, certain failure. */
		dummy = get_dummy_hash();
		if (dummy != NULL)
			result = crypto_pwhash_argon2id_str_verify(dummy, password, strlen(password));
		return FALSE;
	}

	result = crypto_pwhash_argon2id_str_verify(stored, password, strlen(password));
	return result == 0;
}

gboolean
axis_password_needs_rehash(const gchar *stored)
{
	gsize ops;
	gsize mem;

	if (stored == NULL)
		return TRUE;

	get_cost(&ops, &mem);
	return crypto_pwhash_argon2id_str_needs_rehash(stored, (unsigned long long)ops, mem) != 0;
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
