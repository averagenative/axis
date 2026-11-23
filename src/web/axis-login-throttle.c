/*
 * axis-login-throttle.c - exponential delay after failed logins
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "web/axis-login-throttle.h"

#include <string.h>

/* A key longer than this is cut (it is only ever compared). */
#define KEY_MAX_LEN 128

typedef struct
{
	guint  failures;
	gint64 last_failure_us;
	gint64 locked_until_us;
	guint  pending;		/* attempts admitted and not yet ended */
} Entry;

struct _AxisLoginThrottle
{
	guint       free_failures;
	gint64      base_delay_us;
	gint64      max_delay_us;
	gint64      forget_us;
	guint       max_keys;
	GHashTable *entries;	/* key -> Entry* */
};

AxisLoginThrottle *
axis_login_throttle_new(
	guint  free_failures,
	gint64 base_delay_us,
	gint64 max_delay_us,
	gint64 forget_us,
	guint  max_keys
){
	AxisLoginThrottle *self = g_new0(AxisLoginThrottle, 1);

	self->free_failures = free_failures;
	self->base_delay_us = MAX(base_delay_us, 1);
	self->max_delay_us = MAX(max_delay_us, self->base_delay_us);
	self->forget_us = MAX(forget_us, self->max_delay_us);
	self->max_keys = MAX(max_keys, 1);
	self->entries = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

	return self;
}

void
axis_login_throttle_free(AxisLoginThrottle *self)
{
	if (self == NULL)
		return;

	g_hash_table_unref(self->entries);
	g_free(self);
}

static gchar *
clip_key(const gchar *key)
{
	return g_strndup(key, KEY_MAX_LEN);
}

/* Whether the failures of @entry are old enough to be forgotten. */
static gboolean
failures_stale(
	AxisLoginThrottle *self,
	const Entry       *entry,
	gint64             now_us
){
	return now_us - entry->last_failure_us >= self->forget_us &&
	       now_us >= entry->locked_until_us;
}

/* An entry is dropped only when nothing about it is left to count. */
static gboolean
is_forgotten(
	AxisLoginThrottle *self,
	const Entry       *entry,
	gint64             now_us
){
	return entry->pending == 0 && failures_stale(self, entry, now_us);
}

/* The delay that @failures in all lock a key for. */
static gint64
delay_after(
	AxisLoginThrottle *self,
	guint              failures
){
	guint doublings;
	gint64 delay = self->base_delay_us;

	if (failures <= self->free_failures)
		return 0;

	doublings = failures - self->free_failures - 1;
	while (doublings > 0 && delay < self->max_delay_us) {
		delay *= 2;
		doublings--;
	}

	return MIN(delay, self->max_delay_us);
}

gint64
axis_login_throttle_check(
	AxisLoginThrottle *self,
	const gchar       *key,
	gint64             now_us
){
	g_autofree gchar *clipped = NULL;
	Entry *entry;
	guint failures;

	g_return_val_if_fail(self != NULL, 0);

	if (key == NULL)
		return 0;

	clipped = clip_key(key);
	entry = g_hash_table_lookup(self->entries, clipped);
	if (entry == NULL)
		return 0;
	if (is_forgotten(self, entry, now_us)) {
		g_hash_table_remove(self->entries, clipped);
		return 0;
	}

	if (entry->locked_until_us > now_us)
		return entry->locked_until_us - now_us;

	/* Attempts still being checked count as the failures they may turn
	 * out to be: one after another, the one that would start a delay is
	 * the last let through until the others have ended. */
	failures = failures_stale(self, entry, now_us) ? 0 : entry->failures;
	if (entry->pending > 0 && failures + entry->pending > self->free_failures)
		return MAX(delay_after(self, failures + entry->pending), G_USEC_PER_SEC);

	return 0;
}

/* Makes room for one more key. */
static void
make_room(
	AxisLoginThrottle *self,
	gint64             now_us
){
	GHashTableIter iter;
	gpointer key;
	gpointer value;
	const gchar *oldest_key = NULL;
	gint64 oldest = G_MAXINT64;

	if (g_hash_table_size(self->entries) < self->max_keys)
		return;

	g_hash_table_iter_init(&iter, self->entries);
	while (g_hash_table_iter_next(&iter, &key, &value)) {
		if (is_forgotten(self, value, now_us))
			g_hash_table_iter_remove(&iter);
	}
	if (g_hash_table_size(self->entries) < self->max_keys)
		return;

	/* An entry with attempts in flight, or still in its delay, is never
	 * the one dropped: its attempts would come back to nothing, or a
	 * flood of other keys would end its delay.  When every entry is so,
	 * the table grows by one (each such entry took failures to make, and
	 * the delays end). */
	g_hash_table_iter_init(&iter, self->entries);
	while (g_hash_table_iter_next(&iter, &key, &value)) {
		const Entry *entry = value;

		if (entry->pending == 0 && entry->locked_until_us <= now_us && entry->last_failure_us < oldest) {
			oldest = entry->last_failure_us;
			oldest_key = key;
		}
	}
	if (oldest_key != NULL)
		g_hash_table_remove(self->entries, oldest_key);
}

/* The entry of @clipped, made (with room) if there is none; takes @clipped
 * when it makes one. */
static Entry *
entry_for(
	AxisLoginThrottle  *self,
	gchar             **clipped,
	gint64              now_us
){
	Entry *entry = g_hash_table_lookup(self->entries, *clipped);

	if (entry == NULL) {
		make_room(self, now_us);
		entry = g_new0(Entry, 1);
		g_hash_table_insert(self->entries, g_steal_pointer(clipped), entry);
	}

	return entry;
}

void
axis_login_throttle_fail(
	AxisLoginThrottle *self,
	const gchar       *key,
	gint64             now_us
){
	g_autofree gchar *clipped = NULL;
	Entry *entry;

	g_return_if_fail(self != NULL);

	if (key == NULL)
		return;

	clipped = clip_key(key);
	entry = g_hash_table_lookup(self->entries, clipped);
	if (entry != NULL && failures_stale(self, entry, now_us)) {
		entry->failures = 0;
		entry->locked_until_us = 0;
	}
	entry = entry_for(self, &clipped, now_us);

	if (entry->failures < G_MAXUINT)
		entry->failures++;
	entry->last_failure_us = now_us;

	if (entry->failures > self->free_failures)
		entry->locked_until_us = MAX(entry->locked_until_us,
			now_us + delay_after(self, entry->failures));
}

void
axis_login_throttle_begin(
	AxisLoginThrottle *self,
	const gchar       *key,
	gint64             now_us
){
	g_autofree gchar *clipped = NULL;
	Entry *entry;

	g_return_if_fail(self != NULL);

	if (key == NULL)
		return;

	clipped = clip_key(key);
	entry = entry_for(self, &clipped, now_us);
	if (entry->pending < G_MAXUINT)
		entry->pending++;
}

void
axis_login_throttle_end(
	AxisLoginThrottle *self,
	const gchar       *key
){
	g_autofree gchar *clipped = NULL;
	Entry *entry;

	g_return_if_fail(self != NULL);

	if (key == NULL)
		return;

	clipped = clip_key(key);
	entry = g_hash_table_lookup(self->entries, clipped);
	if (entry != NULL && entry->pending > 0)
		entry->pending--;
}

void
axis_login_throttle_clear(
	AxisLoginThrottle *self,
	const gchar       *key
){
	g_autofree gchar *clipped = NULL;
	Entry *entry;

	g_return_if_fail(self != NULL);

	if (key == NULL)
		return;

	clipped = clip_key(key);
	entry = g_hash_table_lookup(self->entries, clipped);
	if (entry == NULL)
		return;

	/* Attempts still in flight keep their count; the failures go. */
	if (entry->pending > 0) {
		entry->failures = 0;
		entry->locked_until_us = 0;
		entry->last_failure_us = 0;
		return;
	}
	g_hash_table_remove(self->entries, clipped);
}

guint
axis_login_throttle_get_pending(
	AxisLoginThrottle *self,
	const gchar       *key
){
	g_autofree gchar *clipped = NULL;
	Entry *entry;

	g_return_val_if_fail(self != NULL, 0);

	if (key == NULL)
		return 0;

	clipped = clip_key(key);
	entry = g_hash_table_lookup(self->entries, clipped);
	return entry != NULL ? entry->pending : 0;
}

guint
axis_login_throttle_get_size(AxisLoginThrottle *self)
{
	g_return_val_if_fail(self != NULL, 0);

	return g_hash_table_size(self->entries);
}
