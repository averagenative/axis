/*
 * axis-indexer.c - the indexer layer: lifetime, kinds, settings and the
 * sources' in-memory state
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "indexer/axis-indexer-private.h"

#include <string.h>

G_DEFINE_QUARK(axis-indexer-error-quark, axis_indexer_error)

/* ── Settings ───────────────────────────────────────────────────── */

static const AxisSettingInfo settings_table[] = {
	{ AXIS_INDEXER_SETTING_CACHE_LIFE, AXIS_SETTING_DURATION, "Keep a search's releases for",
	  "A search and the releases it found are kept this long. The same search again within this time is answered "
	  "from what was kept, without asking the indexer; a release is fetched from the list it was found in, so "
	  "this is also how long a release can be grabbed after it was listed.", 0, "15m", 10, 86400, 0, 0, NULL },
	{ AXIS_INDEXER_SETTING_RATE, AXIS_SETTING_DECIMAL, "Requests a second, per indexer",
	  "How often an indexer is asked for something, on average: every search, and every file fetched from it, counts. "
	  "One a second is gentle on the indexers behind it.", 0, "1.00", 0, 0, 0.05, 20.0, NULL },
	{ AXIS_INDEXER_SETTING_BURST, AXIS_SETTING_INTEGER, "Requests allowed before the rate above applies, per indexer",
	  "How many requests to an indexer may go at once before the rate above applies.", 0, "5", 1, 50, 0, 0, NULL },
	{ AXIS_INDEXER_SETTING_MAX_SIMULTANEOUS, AXIS_SETTING_INTEGER, "Requests in progress at one indexer, at most",
	  "At most this many searches or fetches are in progress at one indexer at the same time; others wait their "
	  "turn.", 0, "2", 1, 8, 0, 0, NULL },
	{ AXIS_INDEXER_SETTING_SITE_RATE, AXIS_SETTING_DECIMAL, "Requests a second, per public site",
	  "How often a public torrent site (YTS, EZTV, The Pirate Bay) is asked for something, on average, one request "
	  "at a time. These sites are free and public, so the default is gentle: one request every two seconds.", 0, "0.50", 0, 0, 0.05, 20.0, NULL },
	{ NULL, 0, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL }
};

const AxisSettingInfo *
axis_indexers_get_setting_table(void)
{
	return settings_table;
}

gint64
axis_indexer_setting_duration(
	AxisIndexers *self,
	const gchar  *key,
	gint64        fallback
){
	if (self->settings == NULL || axis_settings_lookup(self->settings, key, NULL) == NULL)
		return fallback;
	return axis_settings_get_duration(self->settings, key);
}

gint64
axis_indexer_setting_integer(
	AxisIndexers *self,
	const gchar  *key,
	gint64        fallback
){
	if (self->settings == NULL || axis_settings_lookup(self->settings, key, NULL) == NULL)
		return fallback;
	return axis_settings_get_integer(self->settings, key);
}

gdouble
axis_indexer_setting_decimal(
	AxisIndexers *self,
	const gchar  *key,
	gdouble       fallback
){
	if (self->settings == NULL || axis_settings_lookup(self->settings, key, NULL) == NULL)
		return fallback;
	return axis_settings_get_decimal(self->settings, key);
}

/* ── Lifetime ───────────────────────────────────────────────────── */

static void
slot_free_entry(gpointer data)
{
	axis_indexer_slot_unref(data);
}

AxisIndexers *
axis_indexers_new(
	AxisDbPool   *pool,
	AxisSettings *settings
){
	AxisIndexers *self;

	g_return_val_if_fail(pool != NULL, NULL);

	self = g_new0(AxisIndexers, 1);
	self->refs = 1;
	self->pool = pool;
	self->settings = settings != NULL ? axis_settings_ref(settings) : NULL;
	self->kinds = g_ptr_array_new();
	self->cancel = g_cancellable_new();
	g_mutex_init(&self->lock);
	g_cond_init(&self->inflight_done);
	self->slots = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, slot_free_entry);
	self->inflight = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	return self;
}

AxisIndexers *
axis_indexers_ref(AxisIndexers *self)
{
	g_return_val_if_fail(self != NULL, NULL);
	g_atomic_int_inc(&self->refs);
	return self;
}

AxisDbPool *
axis_indexers_get_pool(AxisIndexers *self)
{
	g_return_val_if_fail(self != NULL, NULL);
	return self->pool;
}

void
axis_indexers_stop(AxisIndexers *self)
{
	g_return_if_fail(self != NULL);

	if (g_atomic_int_exchange(&self->stopped, 1))
		return;
	g_cancellable_cancel(self->cancel);
	/* Searches waiting for another's answer look again, and see the stop. */
	g_mutex_lock(&self->lock);
	g_cond_broadcast(&self->inflight_done);
	g_mutex_unlock(&self->lock);
}

void
axis_indexers_unref(AxisIndexers *self)
{
	if (self == NULL || !g_atomic_int_dec_and_test(&self->refs))
		return;
	axis_indexers_stop(self);
	g_hash_table_unref(self->slots);
	g_hash_table_unref(self->inflight);
	g_ptr_array_unref(self->kinds);
	g_mutex_clear(&self->lock);
	g_cond_clear(&self->inflight_done);
	g_object_unref(self->cancel);
	if (self->settings != NULL)
		axis_settings_unref(self->settings);
	g_free(self);
}

void
axis_indexers_set_tuning(
	AxisIndexers            *self,
	const AxisIndexerTuning *tuning
){
	self->tuning = *tuning;
}

/* ── Kinds ──────────────────────────────────────────────────────── */

gboolean
axis_indexers_add_kind(
	AxisIndexers               *self,
	const AxisIndexerKindInfo  *info,
	GError                    **error
){
	gboolean has_header;
	gboolean has_param;
	gboolean keyless;

	g_return_val_if_fail(self != NULL && info != NULL, FALSE);

	/* The key goes in a header, or (a native indexer's protocol) in the
	 * query: one of the two, named; a public site's kind has none, and only
	 * such a kind (a native one with an address of its own to start from,
	 * or whose indexers each take theirs from a definition) may have
	 * neither. */
	has_header = info->key_header != NULL && info->key_header[0] != '\0';
	has_param = info->key_param != NULL && info->key_param[0] != '\0';
	keyless = info->site && info->native && !has_header && !has_param &&
		(info->default_address != NULL || info->definition);
	if (!axis_indexer_kind_name_valid(info->name) || info->label == NULL || info->secret_label == NULL ||
	    (has_header == has_param && !keyless) || (info->site && !keyless) || (info->definition && !info->site) ||
	    info->test == NULL ||
	    info->indexers == NULL || info->search == NULL || info->fetch == NULL ||
	    (info->key_param != NULL && !info->native)) {
		g_set_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_INVALID,
			"the indexer source kind %s is not complete", info->name != NULL ? info->name : "(unnamed)");
		return FALSE;
	}
	if (axis_indexers_find_kind(self, info->name) != NULL) {
		g_set_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_INVALID,
			"there are two indexer source kinds called %s", info->name);
		return FALSE;
	}
	g_ptr_array_add(self->kinds, (gpointer)info);
	return TRUE;
}

guint
axis_indexers_n_kinds(AxisIndexers *self)
{
	return self->kinds->len;
}

const AxisIndexerKindInfo *
axis_indexers_kind_at(
	AxisIndexers *self,
	guint         index
){
	return index < self->kinds->len ? g_ptr_array_index(self->kinds, index) : NULL;
}

const AxisIndexerKindInfo *
axis_indexers_find_kind(
	AxisIndexers *self,
	const gchar  *name
){
	guint i;

	for (i = 0; name != NULL && i < self->kinds->len; i++) {
		const AxisIndexerKindInfo *info = g_ptr_array_index(self->kinds, i);

		if (strcmp(info->name, name) == 0)
			return info;
	}
	return NULL;
}

/* ── The sources' state ─────────────────────────────────────────── */

AxisIndexerSlot *
axis_indexer_slot_get(
	AxisIndexers *self,
	gint64        id
){
	AxisIndexerSlot *slot;

	g_mutex_lock(&self->lock);
	slot = g_hash_table_lookup(self->slots, &id);
	if (slot == NULL) {
		gint64 *key = g_new(gint64, 1);
		gdouble rate = axis_indexer_setting_decimal(self, AXIS_INDEXER_SETTING_RATE, 1.0);
		guint burst = (guint)axis_indexer_setting_integer(self, AXIS_INDEXER_SETTING_BURST, 5);

		*key = id;
		slot = g_new0(AxisIndexerSlot, 1);
		slot->refs = 1;
		slot->id = id;
		slot->limiter = axis_meta_limiter_new(rate, burst);
		g_mutex_init(&slot->lock);
		g_cond_init(&slot->cond);
		g_hash_table_insert(self->slots, key, slot);
	}
	g_atomic_int_inc(&slot->refs);
	g_mutex_unlock(&self->lock);
	return slot;
}

void
axis_indexer_slot_unref(AxisIndexerSlot *slot)
{
	if (slot == NULL || !g_atomic_int_dec_and_test(&slot->refs))
		return;
	axis_meta_limiter_free(slot->limiter);
	g_mutex_clear(&slot->lock);
	g_cond_clear(&slot->cond);
	g_free(slot);
}

void
axis_indexer_forget_source(
	AxisIndexers *self,
	gint64        id
){
	g_mutex_lock(&self->lock);
	g_hash_table_remove(self->slots, &id);
	g_mutex_unlock(&self->lock);
}

gchar *
axis_indexer_contact_problem(
	const GError           *failure,
	AxisIndexerSourceState *state
){
	const gchar *prefix;

	if (g_error_matches(failure, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_AUTH)) {
		*state = AXIS_INDEXER_SOURCE_REFUSED;
		prefix = "The API key was refused";
	} else if (g_error_matches(failure, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_UNREACHABLE)) {
		*state = AXIS_INDEXER_SOURCE_UNREACHABLE;
		prefix = "Not answering";
	} else {
		*state = AXIS_INDEXER_SOURCE_FAILING;
		prefix = "No usable answer";
	}
	return g_strdup_printf("%s: %s.", prefix, failure->message);
}

/* ── Records ────────────────────────────────────────────────────── */

/* How many indexers of @source of @protocol are on, and how many of those
 * redirect their downloads while following is off. */
static void
count_refused(
	const AxisIndexerSourceInfo *source,
	AxisDownloadProtocol         protocol,
	guint                       *on,
	guint                       *refused
){
	guint i;

	*on = 0;
	*refused = 0;
	for (i = 0; source != NULL && source->indexers != NULL && i < source->indexers->len; i++) {
		const AxisIndexerInfo *info = g_ptr_array_index(source->indexers, i);

		if (!info->enabled || info->protocol != protocol)
			continue;
		(*on)++;
		if (info->redirect && !source->follow_redirects)
			(*refused)++;
	}
}

gboolean
axis_indexer_source_redirects_refused(
	const AxisIndexerSourceInfo *source,
	AxisDownloadProtocol         protocol
){
	guint on;
	guint refused;

	count_refused(source, protocol, &on, &refused);
	return refused > 0;
}

gboolean
axis_indexer_source_all_redirects_refused(
	const AxisIndexerSourceInfo *source,
	AxisDownloadProtocol         protocol
){
	guint on;
	guint refused;

	count_refused(source, protocol, &on, &refused);
	return on > 0 && refused == on;
}

void
axis_indexer_source_info_free(AxisIndexerSourceInfo *source)
{
	if (source == NULL)
		return;
	g_free(source->name);
	g_free(source->kind);
	g_free(source->address);
	g_free(source->base_path);
	g_free(source->state_message);
	g_free(source->version);
	g_clear_pointer(&source->indexers, g_ptr_array_unref);
	g_free(source->files_origin);
	g_free(source->files_origin_seen);
	axis_indexer_caps_clear(&source->caps);
	g_free(source->hold_message);
	g_free(source->host);
	g_free(source->definition_name);
	g_free(source);
}

gboolean
axis_indexer_source_spent(
	const AxisIndexerSourceInfo *source,
	gboolean                     automatic,
	gint64                       now_us
){
	if (source == NULL || !source->native)
		return FALSE;
	if (source->held_until_us > now_us)
		return TRUE;
	return axis_indexer_allowance_spent(&source->allowance, automatic);
}

AxisIndexerHoldCause
axis_indexer_hold_cause(
	const AxisIndexerSourceInfo *source,
	gint64                       now_us
){
	if (source == NULL || source->held_until_us <= now_us)
		return AXIS_INDEXER_HOLD_NONE;
	if (source->definition_id == 0)
		return AXIS_INDEXER_HOLD_LIMIT;
	if (source->hold_message != NULL && g_str_has_suffix(source->hold_message, AXIS_INDEXER_HOLD_DRIFT_WORDS))
		return AXIS_INDEXER_HOLD_DRIFT;
	if (source->hold_message != NULL && g_str_has_suffix(source->hold_message, AXIS_INDEXER_HOLD_CHALLENGE_WORDS))
		return AXIS_INDEXER_HOLD_CHALLENGE;
	/* Anything else is not Axis's to lift. */
	return AXIS_INDEXER_HOLD_WAIT;
}

void
axis_indexer_search_part_free(AxisIndexerSearchPart *part)
{
	if (part == NULL)
		return;
	g_free(part->name);
	g_free(part->kind);
	g_free(part->problem);
	g_free(part->not_asked);
	g_free(part);
}

void
axis_indexer_search_result_free(AxisIndexerSearchResult *result)
{
	if (result == NULL)
		return;
	g_strfreev(result->failed);
	g_clear_pointer(&result->releases, g_ptr_array_unref);
	g_clear_pointer(&result->parts, g_ptr_array_unref);
	g_clear_pointer(&result->asked_directly, g_ptr_array_unref);
	g_free(result->not_asked);
	g_free(result);
}

void
axis_indexer_learned_clear(AxisIndexerLearned *learned)
{
	if (learned == NULL)
		return;
	axis_indexer_caps_clear(&learned->caps);
	g_free(learned->other_origin);
	g_free(learned->hold_message);
	memset(learned, 0, sizeof(*learned));
}

void
axis_indexer_target_clear(AxisIndexerTarget *target)
{
	if (target == NULL)
		return;
	g_clear_pointer(&target->info, axis_indexer_source_info_free);
	if (target->secret != NULL)
		memset(target->secret, 0, strlen(target->secret));
	g_clear_pointer(&target->secret, g_free);
	g_clear_pointer(&target->definition, g_free);
}

void
axis_indexer_fetch_target_clear(AxisIndexerFetchTarget *target)
{
	g_free(target->name);
	g_free(target->info_hash);
	if (target->fetch_ref != NULL)
		memset(target->fetch_ref, 0, strlen(target->fetch_ref));
	g_free(target->fetch_ref);
	if (target->magnet != NULL)
		memset(target->magnet, 0, strlen(target->magnet));
	g_free(target->magnet);
	memset(target, 0, sizeof(*target));
}

gchar *
axis_indexer_int_text(gint64 value)
{
	return g_strdup_printf("%" G_GINT64_FORMAT, value);
}
