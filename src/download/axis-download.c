/*
 * axis-download.c - the downloads layer: lifetime, kinds, settings, the
 * clients' in-memory state and the listener
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "download/axis-download-private.h"

#include <string.h>

G_DEFINE_QUARK(axis-download-error-quark, axis_download_error)

/* ── Settings ───────────────────────────────────────────────────── */

static const AxisSettingInfo settings_table[] = {
	{ AXIS_DOWNLOAD_SETTING_ACTIVE_INTERVAL, AXIS_SETTING_DURATION, "Check a download client every, while it downloads",
	  "How often a client with a download in progress is asked how it is going (one request each time).",
	  0, "5s", 2, 600, 0, 0, NULL },
	{ AXIS_DOWNLOAD_SETTING_IDLE_INTERVAL, AXIS_SETTING_DURATION, "Check a download client every, while it is idle",
	  "How often a client with nothing in progress is looked at.", 0, "1m", 10, 3600, 0, 0, NULL },
	{ AXIS_DOWNLOAD_SETTING_STALL_AFTER, AXIS_SETTING_DURATION, "A torrent is stalled after",
	  "A torrent that has not grown for this long is marked stalled. Nothing is done about it here: the "
	  "module that wanted it decides.", 0, "1h", 300, 30 * 86400, 0, 0, NULL },
	{ AXIS_DOWNLOAD_SETTING_GONE_AFTER, AXIS_SETTING_INTEGER, "Mark a download gone after this many checks without it",
	  "A download the client no longer lists is marked gone after it has been missing this many times in a "
	  "row (a client being restarted can list nothing for a moment).", 0, "3", 2, 100, 0, 0, NULL },
	{ NULL, 0, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL }
};

const AxisSettingInfo *
axis_downloads_get_setting_table(void)
{
	return settings_table;
}

gint64
axis_download_setting_duration(
	AxisDownloads *self,
	const gchar   *key,
	gint64         fallback
){
	if (self->settings == NULL || axis_settings_lookup(self->settings, key, NULL) == NULL)
		return fallback;
	return axis_settings_get_duration(self->settings, key);
}

gint64
axis_download_setting_integer(
	AxisDownloads *self,
	const gchar   *key,
	gint64         fallback
){
	if (self->settings == NULL || axis_settings_lookup(self->settings, key, NULL) == NULL)
		return fallback;
	return axis_settings_get_integer(self->settings, key);
}

/* ── Lifetime ───────────────────────────────────────────────────── */

static void
client_state_free(gpointer data)
{
	axis_download_slot_unref(data);
}

AxisDownloads *
axis_downloads_new(
	AxisDbPool   *pool,
	AxisStorage  *storage,
	AxisSettings *settings
){
	AxisDownloads *self;

	g_return_val_if_fail(pool != NULL && storage != NULL, NULL);

	self = g_new0(AxisDownloads, 1);
	self->refs = 1;
	self->pool = pool;
	self->storage = axis_storage_ref(storage);
	self->settings = settings != NULL ? axis_settings_ref(settings) : NULL;
	self->kinds = g_ptr_array_new();
	self->cancel = g_cancellable_new();
	g_mutex_init(&self->lock);
	g_mutex_init(&self->listener_lock);
	self->clients = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, client_state_free);
	self->sending = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	return self;
}

AxisDownloads *
axis_downloads_ref(AxisDownloads *self)
{
	g_return_val_if_fail(self != NULL, NULL);
	g_atomic_int_inc(&self->refs);
	return self;
}

AxisDbPool *
axis_downloads_get_pool(AxisDownloads *self)
{
	g_return_val_if_fail(self != NULL, NULL);
	return self->pool;
}

void
axis_downloads_stop(AxisDownloads *self)
{
	g_return_if_fail(self != NULL);

	if (g_atomic_int_exchange(&self->stopped, 1))
		return;
	g_cancellable_cancel(self->cancel);
	axis_downloads_set_listener(self, NULL, NULL);
}

void
axis_downloads_unref(AxisDownloads *self)
{
	if (self == NULL || !g_atomic_int_dec_and_test(&self->refs))
		return;
	axis_downloads_stop(self);
	g_hash_table_unref(self->clients);
	g_hash_table_unref(self->sending);
	g_ptr_array_unref(self->kinds);
	g_mutex_clear(&self->lock);
	g_mutex_clear(&self->listener_lock);
	g_object_unref(self->cancel);
	axis_storage_unref(self->storage);
	if (self->settings != NULL)
		axis_settings_unref(self->settings);
	g_free(self);
}

void
axis_downloads_set_tuning(
	AxisDownloads            *self,
	const AxisDownloadTuning *tuning
){
	self->tuning = *tuning;
}

void
axis_downloads_set_fault(
	AxisDownloads     *self,
	AxisDownloadFault  fault
){
	g_atomic_int_set(&self->fault, (gint)fault);
}

/* ── Kinds ──────────────────────────────────────────────────────── */

gboolean
axis_downloads_add_kind(
	AxisDownloads               *self,
	const AxisDownloadKindInfo  *info,
	GError                     **error
){
	g_return_val_if_fail(self != NULL && info != NULL, FALSE);

	/* A built-in kind has no secret and no category to name, and alone takes
	 * a protocol no client program has. */
	if (!axis_download_kind_name_valid(info->name) || info->label == NULL || info->test == NULL ||
	    info->add == NULL || info->list == NULL || info->remove == NULL ||
	    (!info->builtin && (info->secret_label == NULL || info->category_label == NULL)) ||
	    (info->builtin ? info->protocol != AXIS_DOWNLOAD_YOUTUBE :
	     (info->protocol != AXIS_DOWNLOAD_USENET && info->protocol != AXIS_DOWNLOAD_TORRENT))) {
		g_set_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID,
			"the download client kind %s is not complete", info->name != NULL ? info->name : "(unnamed)");
		return FALSE;
	}
	if (axis_downloads_find_kind(self, info->name) != NULL) {
		g_set_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID,
			"there are two download client kinds called %s", info->name);
		return FALSE;
	}
	g_ptr_array_add(self->kinds, (gpointer)info);
	return TRUE;
}

guint
axis_downloads_n_kinds(AxisDownloads *self)
{
	return self->kinds->len;
}

const AxisDownloadKindInfo *
axis_downloads_kind_at(
	AxisDownloads *self,
	guint          index
){
	return index < self->kinds->len ? g_ptr_array_index(self->kinds, index) : NULL;
}

const AxisDownloadKindInfo *
axis_downloads_find_kind(
	AxisDownloads *self,
	const gchar   *name
){
	guint i;

	for (i = 0; name != NULL && i < self->kinds->len; i++) {
		const AxisDownloadKindInfo *info = g_ptr_array_index(self->kinds, i);

		if (strcmp(info->name, name) == 0)
			return info;
	}
	return NULL;
}

/* ── The clients' state ─────────────────────────────────────────── */

AxisDownloadSlot *
axis_download_slot_get(
	AxisDownloads *self,
	gint64         id
){
	AxisDownloadSlot *state;

	g_mutex_lock(&self->lock);
	state = g_hash_table_lookup(self->clients, &id);
	if (state == NULL) {
		gint64 *key = g_new(gint64, 1);

		*key = id;
		state = g_new0(AxisDownloadSlot, 1);
		state->refs = 1;
		state->id = id;
		g_mutex_init(&state->call);
		g_hash_table_insert(self->clients, key, state);
	}
	g_atomic_int_inc(&state->refs);
	g_mutex_unlock(&self->lock);
	return state;
}

void
axis_download_slot_unref(AxisDownloadSlot *state)
{
	if (state == NULL || !g_atomic_int_dec_and_test(&state->refs))
		return;
	axis_http_session_free(state->session);
	g_mutex_clear(&state->call);
	g_free(state->folder_warning);
	g_free(state);
}

void
axis_downloads_set_folder_warning(
	AxisDownloads *self,
	gint64         client_id,
	const gchar   *text
){
	AxisDownloadSlot *state = axis_download_slot_get(self, client_id);

	g_mutex_lock(&self->lock);
	g_free(state->folder_warning);
	state->folder_warning = g_strdup(text);
	g_mutex_unlock(&self->lock);
	axis_download_slot_unref(state);
}

gchar *
axis_download_folder_warning(
	AxisDownloads *self,
	gint64         client_id
){
	AxisDownloadSlot *state = axis_download_slot_get(self, client_id);
	gchar *text;

	g_mutex_lock(&self->lock);
	text = g_strdup(state->folder_warning);
	g_mutex_unlock(&self->lock);
	axis_download_slot_unref(state);
	return text;
}

void
axis_download_note_partial(
	AxisDownloads *self,
	gint64         client_id,
	gboolean       partial
){
	AxisDownloadSlot *state = axis_download_slot_get(self, client_id);

	g_mutex_lock(&self->lock);
	if (!partial)
		state->partial_since_us = 0;
	else if (state->partial_since_us == 0)
		state->partial_since_us = g_get_real_time();
	g_mutex_unlock(&self->lock);
	axis_download_slot_unref(state);
}

gint64
axis_download_partial_since(
	AxisDownloads *self,
	gint64         client_id
){
	AxisDownloadSlot *state = axis_download_slot_get(self, client_id);
	gint64 since;

	g_mutex_lock(&self->lock);
	since = state->partial_since_us;
	g_mutex_unlock(&self->lock);
	axis_download_slot_unref(state);
	return since;
}

void
axis_download_forget_client(
	AxisDownloads *self,
	gint64         id
){
	AxisDownloadSlot *state = axis_download_slot_get(self, id);

	/* The next call makes a new session, with no cookie of the old one. */
	g_mutex_lock(&state->call);
	g_clear_pointer(&state->session, axis_http_session_free);
	state->session_stamp = 0;
	g_mutex_unlock(&state->call);
	g_mutex_lock(&self->lock);
	state->next_poll_us = 0;
	g_mutex_unlock(&self->lock);
	axis_download_slot_unref(state);
}

gboolean
axis_download_is_sending(
	AxisDownloads *self,
	gint64         id
){
	gboolean sending;

	g_mutex_lock(&self->lock);
	sending = g_hash_table_contains(self->sending, &id);
	g_mutex_unlock(&self->lock);
	return sending;
}

void
axis_download_mark_sending(
	AxisDownloads *self,
	gint64         id,
	gboolean       sending
){
	g_mutex_lock(&self->lock);
	if (sending) {
		gint64 *key = g_new(gint64, 1);

		*key = id;
		g_hash_table_add(self->sending, key);
	} else {
		g_hash_table_remove(self->sending, &id);
	}
	g_mutex_unlock(&self->lock);
}

gchar *
axis_download_contact_problem(
	const GError            *failure,
	AxisDownloadClientState *state
){
	const gchar *prefix;

	if (g_error_matches(failure, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_AUTH)) {
		*state = AXIS_DOWNLOAD_CLIENT_REFUSED;
		prefix = "The API key or password was refused";
	} else if (g_error_matches(failure, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_UNREACHABLE)) {
		*state = AXIS_DOWNLOAD_CLIENT_UNREACHABLE;
		prefix = "Not answering";
	} else {
		*state = AXIS_DOWNLOAD_CLIENT_FAILING;
		prefix = "No usable answer";
	}
	return g_strdup_printf("%s: %s.", prefix, failure->message);
}

/* ── The listener ───────────────────────────────────────────────── */

void
axis_downloads_set_listener(
	AxisDownloads         *self,
	AxisDownloadsListener  listener,
	gpointer               user_data
){
	g_return_if_fail(self != NULL);

	g_mutex_lock(&self->listener_lock);
	self->listener = listener;
	self->listener_data = user_data;
	g_mutex_unlock(&self->listener_lock);
}

void
axis_download_notify(
	AxisDownloads *self,
	GArray        *ids,
	gboolean       set_changed
){
	if ((ids == NULL || ids->len == 0) && !set_changed)
		return;
	/* Held during the call, so that a listener removed is never called
	 * afterwards; the listener only copies and returns. */
	g_mutex_lock(&self->listener_lock);
	if (self->listener != NULL) {
		g_autoptr(GArray) empty = NULL;

		if (ids == NULL)
			ids = empty = g_array_new(FALSE, FALSE, sizeof(gint64));
		self->listener(ids, set_changed, self->listener_data);
	}
	g_mutex_unlock(&self->listener_lock);
}

/* ── Records ────────────────────────────────────────────────────── */

void
axis_download_path_map_info_free(AxisDownloadPathMapInfo *map)
{
	if (map == NULL)
		return;
	g_free(map->client_prefix);
	g_free(map->volume_name);
	g_free(map->rel_path);
	g_free(map);
}

void
axis_download_client_info_free(AxisDownloadClientInfo *client)
{
	if (client == NULL)
		return;
	g_free(client->name);
	g_free(client->kind);
	g_free(client->address);
	g_free(client->base_path);
	g_free(client->username);
	g_free(client->category);
	g_free(client->state_message);
	g_free(client->version);
	g_free(client->problem);
	g_clear_pointer(&client->maps, g_ptr_array_unref);
	g_free(client);
}

void
axis_download_info_free(AxisDownloadInfo *info)
{
	if (info == NULL)
		return;
	g_free(info->client_name);
	g_free(info->client_ref);
	g_free(info->info_hash);
	g_free(info->release_name);
	g_free(info->title_label);
	g_free(info->module);
	g_free(info->client_path);
	g_free(info->volume_name);
	g_free(info->rel_path);
	g_free(info->message);
	g_free(info->problem);
	g_free(info->folder_removal);
	g_free(info->folder_removal_text);
	g_free(info);
}

gdouble
axis_download_info_progress(const AxisDownloadInfo *info)
{
	if (info->state == AXIS_DOWNLOAD_COMPLETED || info->state == AXIS_DOWNLOAD_IMPORTED)
		return 1.0;
	if (info->size_bytes <= 0 || info->done_bytes < 0)
		return -1.0;
	return CLAMP((gdouble)info->done_bytes / (gdouble)info->size_bytes, 0.0, 1.0);
}

void
axis_download_test_result_clear(AxisDownloadTestResult *result)
{
	if (result == NULL)
		return;
	g_clear_pointer(&result->version, g_free);
	g_clear_pointer(&result->note, g_free);
	result->category_exists = -1;
}

void
axis_download_target_clear(AxisDownloadTarget *target)
{
	if (target == NULL)
		return;
	g_clear_pointer(&target->info, axis_download_client_info_free);
	if (target->secret != NULL)
		memset(target->secret, 0, strlen(target->secret));
	g_clear_pointer(&target->secret, g_free);
}

gchar *
axis_download_int_text(gint64 value)
{
	return g_strdup_printf("%" G_GINT64_FORMAT, value);
}
