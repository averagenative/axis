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
	return axis_secret_init();
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
