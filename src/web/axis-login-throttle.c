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
	/* axis_login_throttle_share_when_full(): */
	gboolean    share_when_full;
	gint64      shared_forget_us;
	Entry       shared;	/* every key that a full table has no room for */
	gint64      pruned_us;	/* when forgotten keys were last looked for */
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
	self->pruned_us = G_MININT64;

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

void
axis_login_throttle_share_when_full(
	AxisLoginThrottle *self,
	gint64             shared_forget_us
){
	g_return_if_fail(self != NULL);

	self->share_when_full = TRUE;
	self->shared_forget_us = MAX(shared_forget_us, self->max_delay_us);
}

static gchar *
clip_key(const gchar *key)
{
	return g_strndup(key, KEY_MAX_LEN);
}

/* Whether the failures of @entry are old enough to be forgotten. */
static gboolean
stale_after(
	const Entry *entry,
	gint64       forget_us,
	gint64       now_us
){
	return now_us - entry->last_failure_us >= forget_us &&
	       now_us >= entry->locked_until_us;
}

static gboolean
failures_stale(
	AxisLoginThrottle *self,
	const Entry       *entry,
	gint64             now_us
){
	return stale_after(entry, entry == &self->shared ? self->shared_forget_us : self->forget_us, now_us);
}

/* An entry is dropped only when nothing about it is left to count: no
 * attempt in flight, and no failure, or none recent. */
static gboolean
is_forgotten(
	AxisLoginThrottle *self,
	const Entry       *entry,
	gint64             now_us
){
	return entry->pending == 0 && (entry->failures == 0 || failures_stale(self, entry, now_us));
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

/* Drops every forgotten key. */
static void
prune(
	AxisLoginThrottle *self,
	gint64             now_us
){
	GHashTableIter iter;
	gpointer value;

	g_hash_table_iter_init(&iter, self->entries);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		if (is_forgotten(self, value, now_us))
			g_hash_table_iter_remove(&iter);
	}
}

/* With axis_login_throttle_share_when_full(): whether a key that is not in
 * the table is counted on the shared entry.  Forgotten keys are looked for
 * at most once a second (they are a day old: a second loses nothing), so
 * that a flood of new keys does not walk the table for each. */
static gboolean
shared_now(
	AxisLoginThrottle *self,
	gint64             now_us
){
	if (!self->share_when_full || g_hash_table_size(self->entries) < self->max_keys)
		return FALSE;
	if (now_us >= self->pruned_us + G_USEC_PER_SEC || now_us < self->pruned_us) {
		self->pruned_us = now_us;
		prune(self, now_us);
	}
	return g_hash_table_size(self->entries) >= self->max_keys;
}

/* What axis_login_throttle_check() answers for @entry. */
static gint64
check_entry(
	AxisLoginThrottle *self,
	Entry             *entry,
	gint64             now_us
){
	guint failures;

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

gint64
axis_login_throttle_check(
	AxisLoginThrottle *self,
	const gchar       *key,
	gint64             now_us
){
	g_autofree gchar *clipped = NULL;
	Entry *entry;

	g_return_val_if_fail(self != NULL, 0);

	if (key == NULL)
		return 0;

	clipped = clip_key(key);
	entry = g_hash_table_lookup(self->entries, clipped);
	if (entry == NULL)
		return shared_now(self, now_us) ? check_entry(self, &self->shared, now_us) : 0;
	if (is_forgotten(self, entry, now_us)) {
		g_hash_table_remove(self->entries, clipped);
		return 0;
	}

	return check_entry(self, entry, now_us);
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

	prune(self, now_us);
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
 * when it makes one.  With axis_login_throttle_share_when_full() and a full
 * table, the shared entry. */
static Entry *
entry_for(
	AxisLoginThrottle  *self,
	gchar             **clipped,
	gint64              now_us
){
	Entry *entry = g_hash_table_lookup(self->entries, *clipped);

	if (entry == NULL) {
		if (shared_now(self, now_us))
			return &self->shared;
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
	entry = entry_for(self, &clipped, now_us);
	if (failures_stale(self, entry, now_us)) {
		entry->failures = 0;
		entry->locked_until_us = 0;
	}

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
	else if (self->share_when_full && self->shared.pending > 0)
		/* (It was begun while the table was full.) */
		self->shared.pending--;
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

	/* A key on the shared entry clears nothing: the others' failures are
	 * counted there too. */
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
