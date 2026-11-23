/*
 * axis-meta.c - the metadata layer: lifetime, providers, settings,
 * freshness, and the reads pages and jobs make
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "meta/axis-meta-private.h"

#include <string.h>

#include "core/axis-error.h"
#include "core/axis-http.h"
#include "library/axis-library-name.h"

/* ── Settings ───────────────────────────────────────────────────── */

static const AxisSettingInfo core_settings[] = {
	{ AXIS_META_SETTING_FRESH_SEARCH, AXIS_SETTING_DURATION, "Title searches stay fresh for",
	  "A search made again within this time is answered from what was saved; after it, the details provider is asked "
	  "again.", 0, "1d", 3600, 90 * 86400, 0, 0, NULL },
	{ AXIS_META_SETTING_FRESH_UNRELEASED, AXIS_SETTING_DURATION, "Details of unreleased movies and continuing series stay fresh for",
	  "How long the record of a movie that is not out yet, or of a series still running, stays fresh.",
	  0, "1d", 3600, 90 * 86400, 0, 0, NULL },
	{ AXIS_META_SETTING_FRESH_RELEASED, AXIS_SETTING_DURATION, "Details of released movies stay fresh for",
	  "How long the record of a released movie stays fresh (seasons other than a series' current one too).",
	  0, "30d", 3600, 365 * 86400, 0, 0, NULL },
	{ AXIS_META_SETTING_FRESH_ENDED, AXIS_SETTING_DURATION, "Details of ended series stay fresh for",
	  "How long the record of a series that has ended stays fresh.", 0, "90d", 3600, 365 * 86400, 0, 0, NULL },
	{ AXIS_META_SETTING_FRESH_SEASON, AXIS_SETTING_DURATION, "Episodes of a current season stay fresh for",
	  "How long the episodes of a running series' latest season stay fresh.", 0, "1d", 3600, 90 * 86400, 0, 0,
	  NULL },
	{ AXIS_META_SETTING_FRESH_NEGATIVE, AXIS_SETTING_DURATION, "Lookups that found nothing are believed for",
	  "How long an answer that found nothing (no such title, no results, an image that was refused) is "
	  "believed.", 0, "1d", 3600, 90 * 86400, 0, 0, NULL },
	{ AXIS_META_SETTING_VIEWED_WINDOW, AXIS_SETTING_DURATION, "Keep viewed titles current for",
	  "A title shown on a page within this time is refreshed on the schedule; one nothing refers to is left "
	  "alone until it is asked for.", 0, "30d", 86400, 3650 * 86400, 0, 0, NULL },
	{ AXIS_META_SETTING_ARTWORK_CAP, AXIS_SETTING_BYTE_SIZE, "Artwork kept, at most",
	  "Posters and backdrops are kept in the data directory up to this size; beyond it, the least recently "
	  "used are removed (they are fetched again when they are shown).", 0, "1GiB", 1024 * 1024,
	  G_GINT64_CONSTANT(1024) * 1024 * 1024 * 1024, 0, 0, NULL }
};

/* Tables live as long as the process (the settings store keeps pointers
 * into them), one per set of providers. */
G_LOCK_DEFINE_STATIC(tables);
static GHashTable *tables;	/* "name,name" -> AxisSettingInfo[] */

/* A key goes in a request header: printable ASCII and no space. */
static gboolean
key_validate(
	const gchar  *text,
	gchar       **reason
){
	const gchar *p;

	for (p = text; *p != '\0'; p++) {
		if ((guchar)*p <= 0x20 || (guchar)*p >= 0x7f) {
			*reason = g_strdup("must be the key as the provider gives it: letters, digits and punctuation, "
				"without spaces");
			return FALSE;
		}
	}
	return TRUE;
}

static const AxisSettingInfo *
build_table(AxisMeta *self)
{
	GString *names = g_string_new(NULL);
	AxisSettingInfo *table;
	guint n_core = G_N_ELEMENTS(core_settings);
	guint i;

	for (i = 0; i < self->providers->len; i++) {
		const AxisMetaProviderEntry *entry = g_ptr_array_index(self->providers, i);

		g_string_append_printf(names, "%s%s", i > 0 ? "," : "", entry->info->name);
	}
	G_LOCK(tables);
	if (tables == NULL)
		tables = g_hash_table_new(g_str_hash, g_str_equal);
	table = g_hash_table_lookup(tables, names->str);
	if (table == NULL) {
		guint n = n_core;
		guint n_extra = 0;

		for (i = 0; i < self->providers->len; i++) {
			const AxisMetaProviderEntry *entry = g_ptr_array_index(self->providers, i);

			n_extra += entry->info->settings != NULL ? entry->info->n_settings : 0;
		}
		table = g_new0(AxisSettingInfo, n_core + 2 * self->providers->len + n_extra + 1);
		memcpy(table, core_settings, sizeof(core_settings));
		for (i = 0; i < self->providers->len; i++) {
			const AxisMetaProviderEntry *entry = g_ptr_array_index(self->providers, i);
			const AxisMetaProviderInfo *info = entry->info;
			gchar rate[G_ASCII_DTOSTR_BUF_SIZE];
			g_autofree gchar *label = NULL;
			g_autofree gchar *help = NULL;
			guint j;

			if (entry->key_setting != NULL) {
				/* A key exchanged for a token travels in the login's body. */
				label = g_strdup_printf(info->login != NULL ? "%s API key" : "%s token", info->label);
				help = info->login != NULL ?
					g_strdup_printf("%s It is never shown again, never written to a log, and sent only to %s, in "
						"the body of its login request; the token the login returns is kept in memory only.",
						info->key_help != NULL ? info->key_help : "The key the provider gave you.", info->api_origin) :
					g_strdup_printf("%s It is never shown again, never written to a log, and sent only to %s, "
					"in a request header.", info->key_help != NULL ? info->key_help :
					"The key the provider gave you.", info->api_origin);
				table[n].key = entry->key_setting;
				table[n].type = AXIS_SETTING_STRING;
				table[n].label = g_intern_string(label);
				table[n].help = g_intern_string(help);
				table[n].flags = AXIS_SETTING_SECRET;
				table[n].maximum = 255;
				n++;
				g_clear_pointer(&label, g_free);
				g_clear_pointer(&help, g_free);
			}
			label = g_strdup_printf("Requests a second (%s)", info->label);
			help = g_strdup_printf("At most this many requests start each second, whoever makes them (searches, "
				"the refresh, artwork). Keep it at or below what %s allows.", info->label);
			g_ascii_formatd(rate, sizeof(rate), "%.2f", info->default_rate);
			table[n].key = entry->rate_setting;
			table[n].type = AXIS_SETTING_DECIMAL;
			table[n].label = g_intern_string(label);
			table[n].help = g_intern_string(help);
			table[n].default_value = g_intern_string(rate);
			table[n].minimum_decimal = 0.05;
			table[n].maximum_decimal = 50.0;
			n++;
			for (j = 0; info->settings != NULL && j < info->n_settings; j++)
				table[n++] = info->settings[j];
		}
		g_hash_table_insert(tables, (gpointer)g_intern_string(names->str), table);
	}
	G_UNLOCK(tables);
	g_string_free(names, TRUE);
	return table;
}

const AxisSettingInfo *
axis_meta_get_setting_table(AxisMeta *self)
{
	guint i;

	g_return_val_if_fail(self != NULL, NULL);

	for (i = 0; i < self->providers->len; i++) {
		const AxisMetaProviderEntry *entry = g_ptr_array_index(self->providers, i);

		if (entry->key_setting != NULL)
			axis_setting_set_validator(entry->key_setting, key_validate);
		/* After the layer's own, so that a provider's check replaces it. */
		if (entry->info->prepare_settings != NULL)
			entry->info->prepare_settings(entry->info);
	}
	return build_table(self);
}

static gint64
duration(
	AxisMeta    *self,
	const gchar *key,
	gint64       fallback
){
	if (self->settings == NULL || axis_settings_lookup(self->settings, key, NULL) == NULL)
		return fallback;
	return axis_settings_get_duration(self->settings, key);
}

void
axis_meta_get_freshness(
	AxisMeta          *self,
	AxisMetaFreshness *freshness
){
	freshness->search = duration(self, AXIS_META_SETTING_FRESH_SEARCH, 86400);
	freshness->unreleased = duration(self, AXIS_META_SETTING_FRESH_UNRELEASED, 86400);
	freshness->released = duration(self, AXIS_META_SETTING_FRESH_RELEASED, 30 * 86400);
	freshness->ended = duration(self, AXIS_META_SETTING_FRESH_ENDED, 90 * 86400);
	freshness->current_season = duration(self, AXIS_META_SETTING_FRESH_SEASON, 86400);
	freshness->negative = duration(self, AXIS_META_SETTING_FRESH_NEGATIVE, 86400);
	freshness->viewed = duration(self, AXIS_META_SETTING_VIEWED_WINDOW, 30 * 86400);
}

gint64
axis_meta_title_period(
	const AxisMetaTitle     *title,
	const AxisMetaFreshness *freshness,
	const gchar             *today
){
	if (title->kind == AXIS_META_KIND_SERIES)
		return title->status == AXIS_META_STATUS_ENDED ? freshness->ended : freshness->unreleased;
	if (title->status == AXIS_META_STATUS_RELEASED)
		return freshness->released;
	if (title->status == AXIS_META_STATUS_UNKNOWN && title->release_date != NULL && today != NULL &&
	    strcmp(title->release_date, today) <= 0)
		return freshness->released;
	return freshness->unreleased;
}

/* ── Lifetime ───────────────────────────────────────────────────── */

static void
entry_free(AxisMetaProviderEntry *entry)
{
	axis_http_origin_clear(&entry->api);
	axis_http_origin_clear(&entry->image);
	axis_meta_limiter_free(entry->limiter);
	axis_meta_fetch_clear_provider(entry);
	g_free(entry);
}

static void
reference_free(AxisMetaReference *reference)
{
	g_free(reference->name);
	g_free(reference->sql);
	g_free(reference);
}

AxisMeta *
axis_meta_new(
	AxisDbPool    *pool,
	AxisSettings  *settings,
	AxisDataDir   *data,
	GError       **error
){
	AxisMeta *self;
	gint art_fd = -1;

	g_return_val_if_fail(pool != NULL, NULL);

	if (data != NULL) {
		art_fd = axis_data_dir_subdir(data, "artwork", error);
		if (art_fd < 0)
			return NULL;
	}
	self = g_new0(AxisMeta, 1);
	self->refs = 1;
	self->pool = pool;
	self->settings = settings != NULL ? axis_settings_ref(settings) : NULL;
	self->data = data != NULL ? axis_data_dir_ref(data) : NULL;
	self->art_fd = art_fd;
	self->providers = g_ptr_array_new_with_free_func((GDestroyNotify)entry_free);
	self->references = g_ptr_array_new_with_free_func((GDestroyNotify)reference_free);
	self->cancel = g_cancellable_new();
	g_mutex_init(&self->lock);
	g_cond_init(&self->done);
	self->inflight = g_hash_table_new(g_str_hash, g_str_equal);
	self->answered = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	return self;
}

AxisMeta *
axis_meta_ref(AxisMeta *self)
{
	g_return_val_if_fail(self != NULL, NULL);
	g_atomic_int_inc(&self->refs);
	return self;
}

static gboolean
drop_pending(
	gpointer key,
	gpointer value,
	gpointer user_data
){
	AxisMeta *self = user_data;
	AxisMetaPending *pending = value;

	/* Never run: the thread's reference is dropped here. */
	pending->finished = TRUE;
	pending->ok = FALSE;
	if (pending->error == NULL)
		g_set_error_literal(&pending->error, AXIS_META_ERROR, AXIS_META_ERROR_CANCELLED, "the daemon is stopping");
	axis_meta_pending_unref(self, pending);
	return TRUE;
}

void
axis_meta_stop(AxisMeta *self)
{
	guint i;

	g_return_if_fail(self != NULL);

	if (g_atomic_int_exchange(&self->stopped, 1))
		return;
	g_cancellable_cancel(self->cancel);
	for (i = 0; i < self->providers->len; i++) {
		AxisMetaProviderEntry *entry = g_ptr_array_index(self->providers, i);

		if (entry->threads != NULL) {
			/* Queued questions are dropped, running ones awaited. */
			g_thread_pool_free(entry->threads, TRUE, TRUE);
			entry->threads = NULL;
		}
	}
	g_mutex_lock(&self->lock);
	g_hash_table_foreach_remove(self->inflight, drop_pending, self);
	g_cond_broadcast(&self->done);
	g_mutex_unlock(&self->lock);
}

void
axis_meta_unref(AxisMeta *self)
{
	if (self == NULL || !g_atomic_int_dec_and_test(&self->refs))
		return;
	axis_meta_stop(self);
	g_ptr_array_unref(self->providers);
	g_ptr_array_unref(self->references);
	g_hash_table_unref(self->inflight);
	g_hash_table_unref(self->answered);
	g_mutex_clear(&self->lock);
	g_cond_clear(&self->done);
	g_object_unref(self->cancel);
	if (self->settings != NULL)
		axis_settings_unref(self->settings);
	if (self->data != NULL)
		axis_data_dir_unref(self->data);
	g_free(self);
}

void
axis_meta_set_tuning(
	AxisMeta             *self,
	const AxisMetaTuning *tuning
){
	self->tuning = *tuning;
}

void
axis_meta_set_enabled(
	AxisMeta *self,
	gboolean  enabled
){
	g_atomic_int_set(&self->enabled, enabled ? 1 : 0);
}

gboolean
axis_meta_is_enabled(AxisMeta *self)
{
	return self != NULL && g_atomic_int_get(&self->enabled) != 0;
}

AxisDbPool *
axis_meta_get_pool(AxisMeta *self)
{
	return self->pool;
}

const gchar *
axis_meta_get_data_dir_path(AxisMeta *self)
{
	return self->data != NULL ? axis_data_dir_get_path(self->data) : NULL;
}

/* ── Providers ──────────────────────────────────────────────────── */

gboolean
axis_meta_add_provider(
	AxisMeta                    *self,
	const AxisMetaProviderInfo  *info,
	GError                     **error
){
	AxisMetaProviderEntry *entry;
	g_autofree gchar *problem = NULL;
	g_autofree gchar *key = NULL;
	g_autofree gchar *rate = NULL;

	g_return_val_if_fail(self != NULL && info != NULL, FALSE);

	if (!axis_meta_provider_name_valid(info->name) || info->label == NULL || info->attribution == NULL ||
	    (info->kinds & (AXIS_META_KIND_BIT(AXIS_META_KIND_FILM) | AXIS_META_KIND_BIT(AXIS_META_KIND_SERIES))) == 0 ||
	    info->search == NULL || info->title == NULL || info->find_by_external == NULL || info->image_path == NULL ||
	    ((info->kinds & AXIS_META_KIND_BIT(AXIS_META_KIND_SERIES)) != 0 && info->season == NULL) ||
	    !(info->default_rate > 0.0) ||
	    /* A login sends a key, and its token goes in the key's header. */
	    (info->login != NULL && info->key_header == NULL) ||
	    (info->pin_setting != NULL && info->login == NULL) ||
	    ((info->order != NULL) != (info->sources != NULL && info->sources[0] != NULL))) {
		g_set_error(error, AXIS_META_ERROR, AXIS_META_ERROR_INVALID, "the metadata provider %s is not complete",
			info->name != NULL ? info->name : "(unnamed)");
		return FALSE;
	}
	if (info->sources != NULL) {
		guint i;

		for (i = 0; info->sources[i] != NULL; i++) {
			g_autofree gchar *owner = axis_meta_source_provider(info->sources[i]);

			if (!axis_meta_source_valid(info->sources[i]) || g_strcmp0(owner, info->name) != 0) {
				g_set_error(error, AXIS_META_ERROR, AXIS_META_ERROR_INVALID,
					"the metadata provider %s names a source of numbering that is not its own", info->name);
				return FALSE;
			}
		}
	}
	if (axis_meta_entry_find(self, info->name) != NULL) {
		g_set_error(error, AXIS_META_ERROR, AXIS_META_ERROR_INVALID, "there are two metadata providers called %s",
			info->name);
		return FALSE;
	}
	entry = g_new0(AxisMetaProviderEntry, 1);
	entry->info = info;
	g_mutex_init(&entry->token_lock);
	g_cond_init(&entry->login_done);
	if (info->pin_setting != NULL)
		entry->pin_setting = g_intern_string(info->pin_setting);
	if (!axis_http_origin_parse(info->api_origin, &entry->api, &problem) ||
	    !axis_http_origin_parse(info->image_origin, &entry->image, &problem)) {
		g_set_error(error, AXIS_META_ERROR, AXIS_META_ERROR_INVALID, "an address of the metadata provider %s %s",
			info->name, problem != NULL ? problem : "is not valid");
		entry_free(entry);
		return FALSE;
	}
	entry->limiter = axis_meta_limiter_new(info->default_rate, info->burst);
	if (info->key_header != NULL) {
		key = g_strdup_printf("%s.%s_key", AXIS_META_MODULE_ID, info->name);
		entry->key_setting = g_intern_string(key);
	}
	rate = g_strdup_printf("%s.%s_rate", AXIS_META_MODULE_ID, info->name);
	entry->rate_setting = g_intern_string(rate);
	axis_meta_fetch_init_provider(self, entry);
	g_ptr_array_add(self->providers, entry);
	return TRUE;
}

AxisMetaProviderEntry *
axis_meta_entry_find(
	AxisMeta    *self,
	const gchar *name
){
	guint i;

	for (i = 0; name != NULL && i < self->providers->len; i++) {
		AxisMetaProviderEntry *entry = g_ptr_array_index(self->providers, i);

		if (strcmp(entry->info->name, name) == 0)
			return entry;
	}
	return NULL;
}

guint
axis_meta_n_providers(AxisMeta *self)
{
	return self->providers->len;
}

const AxisMetaProviderInfo *
axis_meta_provider_at(
	AxisMeta *self,
	guint     index
){
	const AxisMetaProviderEntry *entry;

	if (index >= self->providers->len)
		return NULL;
	entry = g_ptr_array_index(self->providers, index);
	return entry->info;
}

const AxisMetaProviderInfo *
axis_meta_provider_find(
	AxisMeta    *self,
	const gchar *name
){
	const AxisMetaProviderEntry *entry = axis_meta_entry_find(self, name);

	return entry != NULL ? entry->info : NULL;
}

void
axis_meta_provider_status(
	AxisMeta               *self,
	guint                   index,
	AxisMetaProviderStatus *status
){
	AxisMetaProviderEntry *entry;

	memset(status, 0, sizeof(*status));
	if (index >= self->providers->len)
		return;
	entry = g_ptr_array_index(self->providers, index);
	status->info = entry->info;
	status->needs_key = entry->key_setting != NULL;
	status->key_set = status->needs_key && self->settings != NULL &&
		axis_settings_lookup(self->settings, entry->key_setting, NULL) != NULL &&
		axis_settings_is_set(self->settings, entry->key_setting);
	if (self->settings != NULL && axis_settings_lookup(self->settings, entry->rate_setting, NULL) != NULL)
		axis_meta_limiter_set_rate(entry->limiter, axis_settings_get_decimal(self->settings, entry->rate_setting),
			entry->info->burst);
	axis_meta_limiter_get_status(entry->limiter, &status->limit);
	axis_meta_fetch_login_status(self, entry, status);
}

void
axis_meta_provider_status_clear(AxisMetaProviderStatus *status)
{
	if (status == NULL)
		return;
	axis_meta_limit_status_clear(&status->limit);
	g_clear_pointer(&status->test_text, g_free);
}

gboolean
axis_meta_provider_offered(
	AxisMeta                   *self,
	const AxisMetaProviderInfo *info
){
	const AxisMetaProviderEntry *entry;

	g_return_val_if_fail(self != NULL, FALSE);

	if (info == NULL || (entry = axis_meta_entry_find(self, info->name)) == NULL)
		return FALSE;
	if (!info->secondary || entry->key_setting == NULL)
		return TRUE;
	return self->settings != NULL && axis_settings_lookup(self->settings, entry->key_setting, NULL) != NULL &&
		axis_settings_is_set(self->settings, entry->key_setting);
}

void
axis_meta_set_followed(
	AxisMeta           *self,
	const gchar *const *sources
){
	guint i;
	guint k;

	g_return_if_fail(self != NULL);

	for (i = 0; i < self->providers->len; i++) {
		AxisMetaProviderEntry *entry = g_ptr_array_index(self->providers, i);
		gboolean followed = FALSE;

		for (k = 0; sources != NULL && sources[k] != NULL && !followed; k++) {
			g_autofree gchar *owner = axis_meta_source_valid(sources[k]) ? axis_meta_source_provider(sources[k]) :
				NULL;

			followed = g_strcmp0(owner, entry->info->name) == 0;
		}
		g_atomic_int_set(&entry->followed, followed ? 1 : 0);
	}
}

gboolean
axis_meta_provider_followed(
	AxisMeta                   *self,
	const AxisMetaProviderInfo *info
){
	AxisMetaProviderEntry *entry;

	g_return_val_if_fail(self != NULL, FALSE);

	if (info == NULL || (entry = axis_meta_entry_find(self, info->name)) == NULL)
		return FALSE;
	return g_atomic_int_get(&entry->followed) != 0;
}

/* ── References ─────────────────────────────────────────────────── */

gboolean
axis_meta_add_reference(
	AxisMeta    *self,
	const gchar *name,
	const gchar *sql
){
	AxisMetaReference *reference;

	g_return_val_if_fail(self != NULL && name != NULL, FALSE);

	if (sql == NULL || !g_str_has_prefix(sql, "SELECT ") || strchr(sql, ';') != NULL || strlen(sql) > 2000) {
		g_critical("the metadata reference %s is not one SELECT statement", name);
		return FALSE;
	}
	reference = g_new0(AxisMetaReference, 1);
	reference->name = g_strdup(name);
	reference->sql = g_strdup(sql);
	g_ptr_array_add(self->references, reference);
	return TRUE;
}

/* ── Answers ────────────────────────────────────────────────────── */

void
axis_meta_search_answer_clear(AxisMetaSearchAnswer *answer)
{
	if (answer == NULL)
		return;
	g_clear_pointer(&answer->titles, g_ptr_array_unref);
	g_clear_pointer(&answer->problem, g_free);
	memset(answer, 0, sizeof(*answer));
}

void
axis_meta_title_answer_clear(AxisMetaTitleAnswer *answer)
{
	if (answer == NULL)
		return;
	g_clear_pointer(&answer->title, axis_meta_title_free);
	g_clear_pointer(&answer->problem, g_free);
	memset(answer, 0, sizeof(*answer));
}

void
axis_meta_season_answer_clear(AxisMetaSeasonAnswer *answer)
{
	if (answer == NULL)
		return;
	g_clear_pointer(&answer->season, axis_meta_season_free);
	g_clear_pointer(&answer->problem, g_free);
	memset(answer, 0, sizeof(*answer));
}

static gboolean
invalid(
	GError      **error,
	const gchar  *message
){
	g_set_error_literal(error, AXIS_META_ERROR, AXIS_META_ERROR_INVALID, message);
	return FALSE;
}

/* The provider called @name, serving @kind. */
static AxisMetaProviderEntry *
provider_for(
	AxisMeta      *self,
	const gchar   *name,
	AxisMetaKind   kind,
	GError       **error
){
	AxisMetaProviderEntry *entry = axis_meta_entry_find(self, name);

	if (entry == NULL) {
		invalid(error, "there is no such metadata provider");
		return NULL;
	}
	if (axis_meta_kind_to_string(kind) == NULL || (entry->info->kinds & AXIS_META_KIND_BIT(kind)) == 0) {
		g_set_error(error, AXIS_META_ERROR, AXIS_META_ERROR_INVALID, "%s does not know about %s", entry->info->label,
			kind == AXIS_META_KIND_SERIES ? "series" : "movies");
		return NULL;
	}
	return entry;
}

static gboolean
stale(
	gint64 fetched_us,
	gint64 period_s
){
	return fetched_us <= 0 || g_get_real_time() - fetched_us > period_s * G_USEC_PER_SEC;
}

/* What a provider not answering leaves for the page: a message safe to
 * show (the messages here never hold a key).  Only the provider's and the
 * outbound client's own messages are shown; anything else (a database
 * error, with its SQL) goes to the log and the page gets a plain line. */
static gchar *
problem_text(const GError *error)
{
	if (error == NULL)
		return NULL;
	if (error->domain != AXIS_META_ERROR && error->domain != AXIS_HTTP_ERROR) {
		g_message("metadata: an answer could not be read or kept: %s", error->message);
		return g_strdup("Axis could not read or keep the answer; the log has the details");
	}
	return axis_library_text_clean(error->message, 300);
}

/* A saved lookup and its titles, into @answer. */
static gboolean
read_lookup(
	AxisMeta              *self,
	AxisMetaProviderEntry *entry,
	AxisMetaKind           kind,
	const gchar           *lookup,
	const gchar           *query,
	gint                   year,
	gboolean              *found,
	AxisMetaSearchAnswer  *answer,
	GError               **error
){
	g_autoptr(GArray) ids = NULL;
	g_autoptr(GError) local = NULL;
	AxisDbConn *conn = axis_db_pool_acquire(self->pool, &local);
	gboolean ok;

	if (conn == NULL) {
		g_propagate_error(error, g_steal_pointer(&local));
		return FALSE;
	}
	g_clear_pointer(&answer->titles, g_ptr_array_unref);
	ok = axis_meta_store_get_lookup(conn, entry->info->name, kind, lookup, query, year, found, &ids,
		&answer->fetched_us, error) &&
		axis_meta_store_load_titles(conn, ids, &answer->titles, error);
	axis_db_pool_release(self->pool, conn);
	return ok;
}

static gboolean
lookup_answer(
	AxisMeta              *self,
	AxisMetaProviderEntry *entry,
	AxisMetaKind           kind,
	const gchar           *lookup,
	const gchar           *query,
	gint                   year,
	AxisMetaPending       *pending,
	guint                  wait_ms,
	AxisMetaSearchAnswer  *answer,
	GError               **error
){
	AxisMetaFreshness freshness;
	g_autoptr(GError) asked = NULL;
	gboolean found = FALSE;

	axis_meta_get_freshness(self, &freshness);
	answer->provider = entry->info;
	if (!read_lookup(self, entry, kind, lookup, query, year, &found, answer, error)) {
		axis_meta_pending_unref(self, pending);
		return FALSE;
	}
	if (found && !stale(answer->fetched_us, answer->titles->len == 0 ? freshness.negative : freshness.search)) {
		axis_meta_pending_unref(self, pending);
		return TRUE;
	}
	if (axis_meta_ask(self, pending, wait_ms, &asked)) {
		if (!read_lookup(self, entry, kind, lookup, query, year, &found, answer, error))
			return FALSE;
	} else if (wait_ms > 0) {
		answer->saved = TRUE;
		answer->problem = problem_text(asked);
	}
	answer->stale = found && stale(answer->fetched_us, answer->titles->len == 0 ? freshness.negative :
		freshness.search);
	return TRUE;
}

gboolean
axis_meta_search(
	AxisMeta              *self,
	const gchar           *provider,
	AxisMetaKind           kind,
	const gchar           *text,
	gint                   year,
	guint                  wait_ms,
	AxisMetaSearchAnswer  *answer,
	GError               **error
){
	return axis_meta_search_page(self, provider, kind, text, year, 1, wait_ms, answer, error);
}

gboolean
axis_meta_search_page(
	AxisMeta              *self,
	const gchar           *provider,
	AxisMetaKind           kind,
	const gchar           *text,
	gint                   year,
	guint                  page,
	guint                  wait_ms,
	AxisMetaSearchAnswer  *answer,
	GError               **error
){
	AxisMetaProviderEntry *entry;
	g_autofree gchar *sent = NULL;
	g_autofree gchar *key = NULL;
	AxisMetaPending *pending;

	g_return_val_if_fail(self != NULL && answer != NULL, FALSE);

	memset(answer, 0, sizeof(*answer));
	entry = provider_for(self, provider, kind, error);
	if (entry == NULL)
		return FALSE;
	if (page < 1 || page > 2)
		return invalid(error, "only the first two pages of a search are asked for");
	sent = axis_meta_query_clean(text);
	key = axis_meta_query_key(text);
	if (sent == NULL || key == NULL)
		return invalid(error, "there is nothing to look for");
	if (year != 0 && !axis_meta_year_valid(year))
		return invalid(error, "that is not a year");

	pending = axis_meta_pending_new(AXIS_META_ASK_SEARCH, entry);
	pending->key = g_strdup_printf("%s:%s:%d:%d:%s", page > 1 ? "search2" : "search", entry->info->name, (gint)kind,
		year, key);
	pending->kind = kind;
	pending->text = g_steal_pointer(&sent);
	pending->query = g_strdup(key);
	pending->number = year;
	pending->page = page;
	return lookup_answer(self, entry, kind, page > 1 ? "search2" : "search", key, year, pending, wait_ms, answer,
		error);
}

gboolean
axis_meta_find(
	AxisMeta              *self,
	const gchar           *provider,
	AxisMetaKind           kind,
	const gchar           *scheme,
	const gchar           *value,
	guint                  wait_ms,
	AxisMetaSearchAnswer  *answer,
	GError               **error
){
	AxisMetaProviderEntry *entry;
	g_autofree gchar *query = NULL;
	AxisMetaPending *pending;

	g_return_val_if_fail(self != NULL && answer != NULL, FALSE);

	memset(answer, 0, sizeof(*answer));
	entry = provider_for(self, provider, kind, error);
	if (entry == NULL)
		return FALSE;
	if (!axis_meta_id_valid(scheme, value))
		return invalid(error, "that is not an identifier");
	query = g_strdup_printf("%s:%s", scheme, value);
	pending = axis_meta_pending_new(AXIS_META_ASK_FIND, entry);
	pending->key = g_strdup_printf("find:%s:%d:%s", entry->info->name, (gint)kind, query);
	pending->kind = kind;
	pending->text = g_strdup(scheme);
	pending->query = g_strdup(value);
	return lookup_answer(self, entry, kind, "find", query, 0, pending, wait_ms, answer, error);
}

/* The stored record of @entry's title @id: *@title_out and whether a
 * fresh negative answer is saved for it. */
static gboolean
read_title(
	AxisMeta              *self,
	AxisMetaProviderEntry *entry,
	AxisMetaKind           kind,
	const gchar           *id,
	gboolean               mark_viewed,
	AxisMetaTitle        **title_out,
	gboolean              *negative,
	gchar                **today,
	gint64                *asked_us,
	GError               **error
){
	AxisMetaFreshness freshness;
	g_autoptr(GArray) ids = NULL;
	AxisDbConn *conn;
	gint64 title_id = 0;
	gint64 fetched = 0;
	gboolean found = FALSE;
	gboolean ok;

	*title_out = NULL;
	*negative = FALSE;
	*asked_us = 0;
	axis_meta_get_freshness(self, &freshness);
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = axis_meta_store_find_id(conn, kind, entry->info->name, id, &title_id, error);
	if (ok && title_id > 0)
		ok = axis_meta_store_load_title(conn, title_id, TRUE, title_out, error);
	if (ok && title_id > 0 && mark_viewed)
		ok = axis_meta_store_mark_viewed(conn, title_id, error);
	if (ok)
		ok = axis_meta_store_get_lookup(conn, entry->info->name, kind, "title", id, 0, &found, &ids, &fetched,
			error);
	if (ok && today != NULL && *today == NULL)
		*today = axis_meta_store_today(conn, NULL);
	axis_db_pool_release(self->pool, conn);
	if (ok && found && ids->len == 0 && !stale(fetched, freshness.negative))
		*negative = TRUE;
	if (ok && found)
		*asked_us = fetched;
	return ok;
}

/* When @entry's own record of @title was fetched: the record's time when it
 * is the record provider, else when that provider last answered for it (a
 * secondary provider's answer does not touch the record of another). */
static gint64
answered_us(
	const AxisMetaProviderEntry *entry,
	const AxisMetaTitle         *title,
	gint64                       asked_us
){
	if (g_strcmp0(title->provider, entry->info->name) == 0)
		return title->detail_fetched_us;
	return asked_us;
}

static gboolean
title_answer(
	AxisMeta              *self,
	AxisMetaProviderEntry *entry,
	AxisMetaKind           kind,
	const gchar           *id,
	guint                  wait_ms,
	AxisMetaTitleAnswer   *answer,
	GError               **error
){
	AxisMetaFreshness freshness;
	g_autofree gchar *today = NULL;
	g_autoptr(GError) asked = NULL;
	gboolean negative = FALSE;
	gint64 asked_us = 0;
	gint64 period;
	gint64 began = g_get_monotonic_time();

	axis_meta_get_freshness(self, &freshness);
	answer->provider = entry->info;
	if (!read_title(self, entry, kind, id, TRUE, &answer->title, &negative, &today, &asked_us, error))
		return FALSE;
	period = answer->title != NULL ? axis_meta_title_period(answer->title, &freshness, today) : 0;
	if (negative && answer->title == NULL) {
		answer->not_found = TRUE;
		return TRUE;
	}
	if (answer->title == NULL || stale(answered_us(entry, answer->title, asked_us), period)) {
		AxisMetaPending *pending = axis_meta_pending_new(AXIS_META_ASK_TITLE, entry);

		pending->began_us = began;
		pending->key = g_strdup_printf("title:%s:%d:%s", entry->info->name, (gint)kind, id);
		pending->kind = kind;
		pending->text = g_strdup(id);
		if (axis_meta_ask(self, pending, wait_ms, &asked)) {
			g_clear_pointer(&answer->title, axis_meta_title_free);
			/* Viewed now, if it was not stored before. */
			if (!read_title(self, entry, kind, id, TRUE, &answer->title, &negative, &today, &asked_us, error))
				return FALSE;
		} else if (g_error_matches(asked, AXIS_META_ERROR, AXIS_META_ERROR_NOT_FOUND)) {
			answer->not_found = TRUE;
		} else if (wait_ms > 0) {
			answer->saved = TRUE;
			answer->problem = problem_text(asked);
		}
	}
	if (answer->title != NULL) {
		gint64 own = answered_us(entry, answer->title, asked_us);

		period = axis_meta_title_period(answer->title, &freshness, today);
		answer->fetched_us = own > 0 ? own : answer->title->fetched_us;
		answer->stale = stale(own, period);
	}
	return TRUE;
}

gboolean
axis_meta_lookup(
	AxisMeta             *self,
	const gchar          *provider,
	AxisMetaKind          kind,
	const gchar          *id,
	guint                 wait_ms,
	AxisMetaTitleAnswer  *answer,
	GError              **error
){
	AxisMetaProviderEntry *entry;

	g_return_val_if_fail(self != NULL && answer != NULL, FALSE);

	memset(answer, 0, sizeof(*answer));
	entry = provider_for(self, provider, kind, error);
	if (entry == NULL)
		return FALSE;
	if (!axis_meta_id_valid(entry->info->name, id))
		return invalid(error, "that is not one of the provider's identifiers");
	return title_answer(self, entry, kind, id, wait_ms, answer, error);
}

gboolean
axis_meta_get_title(
	AxisMeta             *self,
	gint64                title_id,
	guint                 wait_ms,
	AxisMetaTitleAnswer  *answer,
	GError              **error
){
	g_autoptr(AxisMetaTitle) stored = NULL;
	AxisMetaProviderEntry *entry;
	AxisDbConn *conn;
	gboolean ok;

	g_return_val_if_fail(self != NULL && answer != NULL, FALSE);

	memset(answer, 0, sizeof(*answer));
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = axis_meta_store_load_title(conn, title_id, FALSE, &stored, error);
	axis_db_pool_release(self->pool, conn);
	if (!ok)
		return FALSE;
	if (stored == NULL) {
		g_set_error_literal(error, AXIS_META_ERROR, AXIS_META_ERROR_NOT_FOUND, "there is no such title");
		return FALSE;
	}
	entry = axis_meta_entry_find(self, stored->provider);
	if (entry == NULL) {
		/* Its provider is gone from this version: what is stored is all. */
		conn = axis_db_pool_acquire(self->pool, error);
		if (conn == NULL)
			return FALSE;
		ok = axis_meta_store_load_title(conn, title_id, TRUE, &answer->title, error) &&
			axis_meta_store_mark_viewed(conn, title_id, error);
		axis_db_pool_release(self->pool, conn);
		if (ok && answer->title != NULL)
			answer->fetched_us = answer->title->fetched_us;
		return ok;
	}
	return title_answer(self, entry, stored->kind, stored->provider_id, wait_ms, answer, error);
}

gboolean
axis_meta_load_titles(
	AxisMeta    *self,
	GArray      *ids,
	GPtrArray  **titles,
	GError     **error
){
	AxisDbConn *conn;
	gboolean ok;

	g_return_val_if_fail(self != NULL && ids != NULL && titles != NULL, FALSE);

	*titles = NULL;
	if (ids->len == 0) {
		*titles = g_ptr_array_new_with_free_func((GDestroyNotify)axis_meta_title_free);
		return TRUE;
	}
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = axis_meta_store_load_titles(conn, ids, titles, error);
	axis_db_pool_release(self->pool, conn);
	return ok;
}

guint
axis_meta_provider_lists(
	const AxisMetaProviderInfo *info,
	AxisMetaKind                kind
){
	if (info == NULL || info->list == NULL)
		return 0;
	if ((info->kinds & AXIS_META_KIND_BIT(kind)) == 0)
		return 0;
	return kind == AXIS_META_KIND_FILM ? info->film_lists : kind == AXIS_META_KIND_SERIES ? info->series_lists : 0;
}

gboolean
axis_meta_get_season(
	AxisMeta              *self,
	gint64                 title_id,
	gint                   number,
	guint                  wait_ms,
	AxisMetaSeasonAnswer  *answer,
	GError               **error
){
	return axis_meta_get_source_season(self, title_id, NULL, number, wait_ms, answer, error);
}

gboolean
axis_meta_load_source_seasons(
	AxisMeta     *self,
	gint64        title_id,
	const gchar  *source,
	gchar       **shown,
	gchar       **followed,
	gint64       *series_id,
	GPtrArray   **seasons,
	GError      **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(AxisDbResult) follows = NULL;
	g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT, title_id);
	const gchar *params[2];
	guint i;

	g_return_val_if_fail(self != NULL && shown != NULL && seasons != NULL, FALSE);

	*shown = NULL;
	*seasons = NULL;
	if (followed != NULL)
		*followed = NULL;
	if (series_id != NULL)
		*series_id = 0;
	if (source != NULL && !axis_meta_source_valid(source))
		return invalid(error, "that is not a source of numbering");
	params[0] = id;
	params[1] = source;
	/* One row at least (the source, NULL columns when it has no season). */
	if (!axis_db_pool_exec(self->pool, "WITH src AS (SELECT coalesce($2, (SELECT sr.episode_source FROM series sr"
	        " WHERE sr.title_id = $1::bigint), (SELECT mt.provider FROM meta_title mt WHERE mt.id = $1::bigint)) AS name)"
	        " SELECT src.name, s.number, s.name, s.air_date::text, s.overview, coalesce(s.episode_count, -1),"
	        " (extract(epoch FROM s.fetched_at) * 1000000)::bigint,"
	        " (extract(epoch FROM s.episodes_fetched_at) * 1000000)::bigint"
	        " FROM src LEFT JOIN meta_season s ON s.title_id = $1::bigint AND s.source = src.name ORDER BY s.number",
	        2, params, &result, error) ||
	    !axis_db_pool_exec(self->pool, "SELECT coalesce((SELECT sr.episode_source FROM series sr WHERE sr.title_id ="
	        " $1::bigint), (SELECT mt.provider FROM meta_title mt WHERE mt.id = $1::bigint)),"
	        " (SELECT sr.id FROM series sr WHERE sr.title_id = $1::bigint)", 1, params, &follows, error))
		return FALSE;
	if (followed != NULL && axis_db_result_get_text(follows, 0, 0) != NULL)
		*followed = g_strdup(axis_db_result_get_text(follows, 0, 0));
	if (series_id != NULL)
		axis_db_result_get_int64(follows, 0, 1, series_id);
	*seasons = g_ptr_array_new_with_free_func((GDestroyNotify)axis_meta_season_free);
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		AxisMetaSeason *season;
		gint64 value = 0;
		const gchar *text;

		if (*shown == NULL && axis_db_result_get_text(result, i, 0) != NULL)
			*shown = g_strdup(axis_db_result_get_text(result, i, 0));
		if (!axis_db_result_get_int64(result, i, 1, &value))
			continue;
		season = axis_meta_season_new((gint)value);
		text = axis_db_result_get_text(result, i, 2);
		season->name = text != NULL ? g_strdup(text) : NULL;
		text = axis_db_result_get_text(result, i, 3);
		season->air_date = text != NULL ? g_strdup(text) : NULL;
		text = axis_db_result_get_text(result, i, 4);
		season->overview = text != NULL ? g_strdup(text) : NULL;
		value = -1;
		axis_db_result_get_int64(result, i, 5, &value);
		season->episode_count = (gint)value;
		value = 0;
		axis_db_result_get_int64(result, i, 6, &value);
		season->fetched_us = value;
		value = 0;
		axis_db_result_get_int64(result, i, 7, &value);
		season->episodes_fetched_us = value;
		g_ptr_array_add(*seasons, season);
	}
	return TRUE;
}

/* The question that fetches season @number of @source of @series (a stored
 * series with its identifiers), or %NULL when no provider can be asked: the
 * source's provider has no identifier of the series, is not here, or does
 * not serve that source.  A provider with several orders is asked for the
 * whole order; another for the one season of its own numbering. */
static AxisMetaPending *
source_question(
	AxisMeta            *self,
	const AxisMetaTitle *series,
	const gchar         *source,
	gint                 number
){
	g_autofree gchar *owner = axis_meta_source_provider(source);
	AxisMetaProviderEntry *entry = axis_meta_entry_find(self, owner);
	const gchar *id = NULL;
	AxisMetaPending *pending;

	/* The record's own identifier when the provider made it, else its
	 * identifier the title holds. */
	if (entry != NULL && g_strcmp0(series->provider, entry->info->name) == 0)
		id = series->provider_id;
	else if (entry != NULL)
		id = axis_meta_title_get_id(series, entry->info->name);
	if (entry == NULL || id == NULL || (entry->info->kinds & AXIS_META_KIND_BIT(AXIS_META_KIND_SERIES)) == 0)
		return NULL;
	if (entry->info->order != NULL) {
		pending = axis_meta_pending_new(AXIS_META_ASK_ORDER, entry);
		pending->key = g_strdup_printf("order:%s:%s:%s", entry->info->name, id, source);
	} else if (strcmp(source, entry->info->name) == 0) {
		pending = axis_meta_pending_new(AXIS_META_ASK_SEASON, entry);
		pending->key = g_strdup_printf("season:%s:%s:%d", entry->info->name, id, number);
		pending->number = number;
	} else {
		return NULL;
	}
	pending->kind = AXIS_META_KIND_SERIES;
	pending->text = g_strdup(id);
	pending->query = g_strdup(source);
	return pending;
}

gboolean
axis_meta_get_source_season(
	AxisMeta              *self,
	gint64                 title_id,
	const gchar           *source,
	gint                   number,
	guint                  wait_ms,
	AxisMetaSeasonAnswer  *answer,
	GError               **error
){
	AxisMetaFreshness freshness;
	g_autoptr(AxisMetaTitle) series = NULL;
	g_autoptr(GError) asked = NULL;
	g_autofree gchar *wanted = NULL;
	AxisMetaPending *pending = NULL;
	AxisDbConn *conn;
	gboolean current = FALSE;
	gboolean ok;
	gint64 period;
	gint64 began = g_get_monotonic_time();

	g_return_val_if_fail(self != NULL && answer != NULL, FALSE);

	memset(answer, 0, sizeof(*answer));
	if (number < 0 || number > 10000)
		return invalid(error, "that is not a season");
	if (source != NULL && !axis_meta_source_valid(source))
		return invalid(error, "that is not a source of numbering");
	axis_meta_get_freshness(self, &freshness);
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = axis_meta_store_load_title(conn, title_id, FALSE, &series, error);
	if (ok && series != NULL) {
		/* Without a source named, the record provider's own seasons. */
		wanted = g_strdup(source != NULL ? source : series->provider);
		ok = axis_meta_store_load_season(conn, title_id, wanted, number, &answer->season, &current, error);
	}
	axis_db_pool_release(self->pool, conn);
	if (!ok)
		return FALSE;
	if (series == NULL || series->kind != AXIS_META_KIND_SERIES) {
		g_set_error_literal(error, AXIS_META_ERROR, AXIS_META_ERROR_NOT_FOUND, "there is no such series");
		return FALSE;
	}
	period = current && series->status != AXIS_META_STATUS_ENDED ? freshness.current_season : freshness.released;
	if (answer->season == NULL || stale(answer->season->episodes_fetched_us, period))
		pending = source_question(self, series, wanted, number);
	if (pending != NULL) {
		gboolean whole = pending->ask == AXIS_META_ASK_ORDER;

		pending->began_us = began;
		if (axis_meta_ask(self, pending, wait_ms, &asked)) {
			g_clear_pointer(&answer->season, axis_meta_season_free);
			conn = axis_db_pool_acquire(self->pool, error);
			if (conn == NULL)
				return FALSE;
			ok = axis_meta_store_load_season(conn, title_id, wanted, number, &answer->season, &current, error);
			axis_db_pool_release(self->pool, conn);
			if (!ok)
				return FALSE;
			/* A whole order fetched without this season: it has none. */
			if (whole && answer->season == NULL)
				answer->not_found = TRUE;
		} else if (g_error_matches(asked, AXIS_META_ERROR, AXIS_META_ERROR_NOT_FOUND)) {
			answer->not_found = TRUE;
		} else if (wait_ms > 0) {
			answer->saved = TRUE;
			answer->problem = problem_text(asked);
		}
	}
	if (answer->season != NULL) {
		period = current && series->status != AXIS_META_STATUS_ENDED ? freshness.current_season : freshness.released;
		answer->fetched_us = answer->season->episodes_fetched_us;
		answer->stale = stale(answer->season->episodes_fetched_us, period);
	}
	return TRUE;
}
