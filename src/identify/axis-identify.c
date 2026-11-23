/*
 * axis-identify.c - the identification layer: its lifetime, settings and
 * owner, small helpers, and the scheduler
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "identify/axis-identify-private.h"

#include <json-glib/json-glib.h>
#include <string.h>

#include "jobs/axis-job-lifecycle.h"
#include "library/axis-analysis-rules.h"
#include "library/axis-library.h"

G_DEFINE_QUARK(axis-identify-error-quark, axis_identify_error)

/* ── Settings ───────────────────────────────────────────────────── */

static const AxisSettingInfo settings_table[] = {
	{ AXIS_IDENTIFY_SETTING_PER_RUN, AXIS_SETTING_INTEGER, "Folders identified per 10-minute run",
	  "Each run looks at this many new or changed movie or series folders (or files at the top of a library "
	  "folder) of one library folder, the newest first, so that a large library is identified over hours and the "
	  "details provider is never "
	  "flooded. A folder looked at once is not looked at again until its videos change.", 0, "25", 1, 500, 0, 0,
	  NULL },
	{ AXIS_IDENTIFY_SETTING_PERCENT, AXIS_SETTING_INTEGER, "Runtime difference allowed for a sure match, in percent",
	  "A movie or series matched by its title and year is linked by itself only when the file runs this close to "
	  "the movie's runtime, or a series' episode file to its episode's (in percent of it), or within the minutes "
	  "below, whichever is larger. Anything further goes to review.", 0, "10", 0, 50, 0, 0, NULL },
	{ AXIS_IDENTIFY_SETTING_MINUTES, AXIS_SETTING_INTEGER, "Runtime difference allowed for a sure match, in minutes",
	  "See the difference in percent above: the larger of the two counts.", 0, "5", 0, 60, 0, 0, NULL },
	{ NULL, 0, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL }
};

const AxisSettingInfo *
axis_identify_get_setting_table(void)
{
	return settings_table;
}

gint64
axis_identify_setting_integer(
	AxisIdentify *self,
	const gchar  *key,
	gint64        fallback
){
	if (self->settings == NULL || axis_settings_lookup(self->settings, key, NULL) == NULL)
		return fallback;
	return axis_settings_get_integer(self->settings, key);
}

/* ── Lifetime ───────────────────────────────────────────────────── */

AxisIdentify *
axis_identify_new(
	AxisDbPool   *pool,
	AxisSettings *settings,
	AxisStorage  *storage,
	AxisMeta     *meta
){
	AxisIdentify *self;

	g_return_val_if_fail(pool != NULL && storage != NULL && meta != NULL, NULL);

	self = g_new0(AxisIdentify, 1);
	self->refs = 1;
	self->pool = pool;
	self->settings = settings != NULL ? axis_settings_ref(settings) : NULL;
	self->storage = axis_storage_ref(storage);
	self->meta = axis_meta_ref(meta);
	g_mutex_init(&self->lock);
	/* What the library holds is kept fresh. */
	axis_meta_add_reference(meta, "library", "SELECT title_id FROM library_identity WHERE state = 'linked'"
		" AND title_id IS NOT NULL");
	return self;
}

AxisIdentify *
axis_identify_ref(AxisIdentify *self)
{
	g_return_val_if_fail(self != NULL, NULL);
	g_atomic_int_inc(&self->refs);
	return self;
}

void
axis_identify_unref(AxisIdentify *self)
{
	if (self == NULL || !g_atomic_int_dec_and_test(&self->refs))
		return;
	axis_storage_unref(self->storage);
	axis_meta_unref(self->meta);
	if (self->settings != NULL)
		axis_settings_unref(self->settings);
	g_mutex_clear(&self->lock);
	g_free(self);
}

/* The slot of @kind's owner: 1 films (also for 0), 2 series. */
static guint
owner_slot(AxisMetaKind kind)
{
	return kind == AXIS_META_KIND_SERIES ? 2 : 1;
}

void
axis_identify_add_owner(
	AxisIdentify                *self,
	const AxisIdentifyOwnerInfo *info,
	gpointer                     data
){
	g_return_if_fail(self != NULL && info != NULL);

	self->owners[owner_slot(info->kind)] = info;
	self->owner_data[owner_slot(info->kind)] = data;
}

const AxisIdentifyOwnerInfo *
axis_identify_owner(
	AxisIdentify *self,
	AxisMetaKind  kind,
	gpointer     *data
){
	guint slot = owner_slot(kind);

	if (data != NULL)
		*data = self->owner_data[slot];
	return self->owners[slot];
}

const gchar *
axis_identify_get_owner_module(AxisIdentify *self)
{
	return axis_identify_get_owner_module_for(self, AXIS_META_KIND_FILM);
}

const gchar *
axis_identify_get_owner_module_for(
	AxisIdentify *self,
	AxisMetaKind  kind
){
	const AxisIdentifyOwnerInfo *owner;

	g_return_val_if_fail(self != NULL, NULL);

	owner = axis_identify_owner(self, kind, NULL);
	return owner != NULL ? owner->module : NULL;
}

AxisMetaKind
axis_identify_root_kind(const gchar *media_type)
{
	if (g_strcmp0(media_type, "movies") == 0)
		return AXIS_META_KIND_FILM;
	if (g_strcmp0(media_type, "tv") == 0)
		return AXIS_META_KIND_SERIES;
	return (AxisMetaKind)0;
}

const gchar *
axis_identify_noun(AxisMetaKind kind)
{
	return kind == AXIS_META_KIND_SERIES ? "series" : "movie";
}

const gchar *
axis_identify_nouns(AxisMetaKind kind)
{
	return kind == AXIS_META_KIND_SERIES ? "series" : "movies";
}

AxisDbPool *
axis_identify_get_pool(AxisIdentify *self)
{
	return self->pool;
}

AxisMeta *
axis_identify_get_meta(AxisIdentify *self)
{
	return self->meta;
}

void
axis_identify_set_tuning(
	AxisIdentify             *self,
	const AxisIdentifyTuning *tuning
){
	g_mutex_lock(&self->lock);
	if (tuning != NULL)
		self->tuning = *tuning;
	else
		memset(&self->tuning, 0, sizeof(self->tuning));
	g_mutex_unlock(&self->lock);
}

void
axis_identify_get_tuning(
	AxisIdentify       *self,
	AxisIdentifyTuning *tuning
){
	g_mutex_lock(&self->lock);
	*tuning = self->tuning;
	g_mutex_unlock(&self->lock);
	if (tuning->wait_ms == 0)
		tuning->wait_ms = AXIS_IDENTIFY_WAIT_MS;
	if (tuning->budget_ms == 0)
		tuning->budget_ms = AXIS_IDENTIFY_RUN_BUDGET_MS;
}

/* ── Small helpers ──────────────────────────────────────────────── */

gchar *
axis_identify_int_text(gint64 value)
{
	return g_strdup_printf("%" G_GINT64_FORMAT, value);
}

gchar *
axis_identify_text_cut(
	const gchar *text,
	guint        max
){
	g_autofree gchar *valid = NULL;
	GString *out;
	const gchar *p;
	guint n = 0;

	if (text == NULL)
		return NULL;
	valid = g_utf8_make_valid(text, -1);
	out = g_string_new(NULL);
	for (p = valid; *p != '\0' && n < max; p = g_utf8_next_char(p), n++) {
		gunichar c = g_utf8_get_char(p);

		if (g_unichar_iscntrl(c) || c == 0x2028 || c == 0x2029)
			g_string_append_c(out, ' ');
		else
			g_string_append_unichar(out, c);
	}
	if (out->len == 0) {
		g_string_free(out, TRUE);
		return NULL;
	}
	return g_string_free(out, FALSE);
}

gint64
axis_identify_row_int(
	AxisDbResult *result,
	guint         row,
	guint         column,
	gint64        fallback
){
	gint64 value = fallback;

	if (!axis_db_result_get_int64(result, row, column, &value))
		return fallback;
	return value;
}

gchar *
axis_identify_row_text(
	AxisDbResult *result,
	guint         row,
	guint         column
){
	const gchar *text = axis_db_result_get_text(result, row, column);

	return text != NULL ? g_strdup(text) : NULL;
}

const AxisMetaProviderInfo *
axis_identify_film_provider(AxisIdentify *self)
{
	return axis_identify_provider_for(self, AXIS_META_KIND_FILM);
}

const AxisMetaProviderInfo *
axis_identify_provider_for(
	AxisIdentify *self,
	AxisMetaKind  kind
){
	guint n = axis_meta_n_providers(self->meta);
	guint i;

	for (i = 0; i < n; i++) {
		const AxisMetaProviderInfo *info = axis_meta_provider_at(self->meta, i);

		/* A secondary provider (TheTVDB) is never the one identified by. */
		if ((info->kinds & AXIS_META_KIND_BIT(kind)) != 0 && !info->secondary)
			return info;
	}
	return NULL;
}

gchar *
axis_identify_reasons_to_json(const GPtrArray *reasons)
{
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonGenerator) generator = json_generator_new();
	g_autoptr(JsonNode) root = NULL;
	guint i;

	json_builder_begin_array(builder);
	for (i = 0; reasons != NULL && i < reasons->len && i < 32; i++) {
		const AxisIdentifyReason *r = g_ptr_array_index(reasons, i);
		g_autofree gchar *text = axis_identify_text_cut(r->text, 600);

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "c");
		json_builder_add_string_value(builder, r->code != NULL ? r->code : "");
		json_builder_set_member_name(builder, "t");
		json_builder_add_string_value(builder, text != NULL ? text : "");
		json_builder_end_object(builder);
	}
	json_builder_end_array(builder);
	root = json_builder_get_root(builder);
	json_generator_set_root(generator, root);
	return json_generator_to_data(generator, NULL);
}

GPtrArray *
axis_identify_reasons_from_json(const gchar *json)
{
	GPtrArray *reasons = g_ptr_array_new_with_free_func((GDestroyNotify)axis_identify_reason_free);
	g_autoptr(JsonParser) parser = json_parser_new();
	JsonArray *array;
	guint i;

	if (json == NULL || !json_parser_load_from_data(parser, json, -1, NULL) ||
	    !JSON_NODE_HOLDS_ARRAY(json_parser_get_root(parser)))
		return reasons;
	array = json_node_get_array(json_parser_get_root(parser));
	for (i = 0; i < json_array_get_length(array) && i < 32; i++) {
		JsonNode *node = json_array_get_element(array, i);
		JsonObject *object = JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
		AxisIdentifyReason *r;

		if (object == NULL)
			continue;
		r = g_new0(AxisIdentifyReason, 1);
		r->code = g_strdup(json_object_has_member(object, "c") ? json_object_get_string_member(object, "c") : "");
		r->text = g_strdup(json_object_has_member(object, "t") ? json_object_get_string_member(object, "t") : "");
		if (r->code == NULL)
			r->code = g_strdup("");
		if (r->text == NULL)
			r->text = g_strdup("");
		g_ptr_array_add(reasons, r);
	}
	return reasons;
}

gboolean
axis_identify_owner_link(
	AxisIdentify  *self,
	AxisMetaKind   kind,
	AxisDbConn    *conn,
	gint64         identity_id,
	gint64         title_id,
	const gchar   *user,
	GError       **error
){
	gpointer data = NULL;
	const AxisIdentifyOwnerInfo *owner = axis_identify_owner(self, kind, &data);

	if (owner == NULL || owner->link == NULL)
		return TRUE;
	return owner->link(data, conn, identity_id, title_id, user, error);
}

gboolean
axis_identify_owner_unlink(
	AxisIdentify  *self,
	AxisMetaKind   kind,
	AxisDbConn    *conn,
	gint64         identity_id,
	const gchar   *user,
	GError       **error
){
	gpointer data = NULL;
	const AxisIdentifyOwnerInfo *owner = axis_identify_owner(self, kind, &data);

	if (owner == NULL || owner->unlink == NULL)
		return TRUE;
	return owner->unlink(data, conn, identity_id, user, error);
}

const gchar *
axis_identify_counted_condition(void)
{
	static gsize once = 0;
	static gchar *condition = NULL;

	if (g_once_init_enter(&once)) {
		condition = g_strdup_printf("(f.kind = 'video' AND f.state IN ('present', 'suspected') AND NOT %s AND NOT %s)",
			axis_leftover_condition("sample"), axis_leftover_condition("extras"));
		g_once_init_leave(&once, 1);
	}
	return condition;
}

/* ── Asking for runs ────────────────────────────────────────────── */

gboolean
axis_identify_enqueue(
	AxisDbConn  *conn,
	gint64       root_id,
	gint         priority,
	gboolean    *created,
	GError     **error
){
	g_autoptr(JsonObject) payload = json_object_new();
	g_autofree gchar *key = g_strdup_printf(AXIS_IDENTIFY_KEY_PREFIX "%" G_GINT64_FORMAT, root_id);
	AxisJobSpec spec;

	json_object_set_int_member(payload, "root", root_id);
	memset(&spec, 0, sizeof(spec));
	spec.kind = AXIS_IDENTIFY_KIND;
	spec.queue = AXIS_LIBRARY_SCAN_QUEUE;
	spec.payload = payload;
	spec.priority = priority;
	spec.dedupe_key = key;
	spec.max_attempts = 2;
	return axis_job_enqueue(conn, &spec, NULL, created, error);
}

gboolean
axis_identify_request(
	AxisIdentify  *self,
	gint64         root_id,
	gboolean      *created,
	GError       **error
){
	g_autofree gchar *root = axis_identify_int_text(root_id);
	g_autoptr(AxisDbResult) found = NULL;
	const gchar *params[1];
	AxisDbConn *conn;
	gboolean ok;

	if (created != NULL)
		*created = FALSE;
	params[0] = root;
	if (!axis_db_pool_exec(self->pool, "SELECT 1 FROM library_root WHERE id = $1::bigint AND media_type IN ('movies',"
	        " 'tv')", 1, params, &found, error))
		return FALSE;
	if (axis_db_result_get_n_rows(found) == 0) {
		g_set_error_literal(error, AXIS_IDENTIFY_ERROR, AXIS_IDENTIFY_ERROR_NOT_FOUND, "there is no such root of movies "
			"or for TV");
		return FALSE;
	}
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	/* Asked for: the wait after a provider that did not answer is over, the
	 * root's and its units'. */
	ok = axis_db_conn_exec(conn, "UPDATE library_identity_root SET retry_after = NULL WHERE root_id = $1::bigint", 1,
		params, NULL, error) &&
		axis_db_conn_exec(conn, "UPDATE library_identity SET retry_at = now() WHERE root_id = $1::bigint"
			" AND retry_at > now() AND NOT held", 1, params, NULL, error) &&
		axis_identify_enqueue(conn, root_id, 10, created, error);
	axis_db_pool_release(self->pool, conn);
	return ok;
}

gboolean
axis_identify_run_schedule(
	AxisIdentify  *self,
	guint         *queued,
	GError       **error
){
	g_autoptr(AxisDbResult) roots = NULL;
	AxisDbConn *conn;
	gboolean ok = TRUE;
	guint i;

	if (queued != NULL)
		*queued = 0;
	/* A root with a complete scan, only when something could have changed
	 * since its last run began (0026_identify_schedule.sql): it never ran, its
	 * provider wait is over, its last run left units to look at, a scan of it
	 * completed since, or a file of it was probed since; also when a unit of
	 * it is to be looked at again (AXIS_IDENTIFY_AGAIN: decided by older
	 * rules, or its own wait for the provider over; index ranges over its
	 * units, 0052_identify_hunt_fixes.sql).  Each is a column compared or an
	 * index range; a quiet root costs no run at all. */
	if (!axis_db_pool_exec(self->pool, "SELECT r.id FROM library_root r JOIN library_root_state s ON s.root_id = r.id"
	        " LEFT JOIN library_identity_root ir ON ir.root_id = r.id"
	        " WHERE r.media_type IN ('movies', 'tv') AND s.complete_generation > 0"
	        " AND (ir.root_id IS NULL OR ir.run_started_at IS NULL"
	        "  OR (ir.retry_after IS NOT NULL AND ir.retry_after <= now())"
	        "  OR (ir.retry_after IS NULL AND (ir.pending > 0 OR ir.seasons_pending > 0"
	        "   OR s.complete_generation > ir.scan_generation"
	        "   OR EXISTS (SELECT 1 FROM media_info mi JOIN library_file f ON f.id = mi.file_id"
	        "    WHERE mi.probed_at > ir.run_started_at AND f.root_id = r.id)"
	        "   OR EXISTS (SELECT 1 FROM library_identity i WHERE i.root_id = r.id"
	        "    AND " AXIS_IDENTIFY_AGAIN "))))"
	        " ORDER BY r.id LIMIT 1000", 0, NULL, &roots, error))
		return FALSE;
	if (axis_db_result_get_n_rows(roots) == 0)
		return TRUE;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	for (i = 0; ok && i < axis_db_result_get_n_rows(roots); i++) {
		gboolean created = FALSE;

		ok = axis_identify_enqueue(conn, axis_identify_row_int(roots, i, 0, 0), 0, &created, error);
		if (ok && created && queued != NULL)
			(*queued)++;
	}
	axis_db_pool_release(self->pool, conn);
	return ok;
}

gboolean
axis_identify_run_continue(
	AxisIdentify  *self,
	guint         *queued,
	GError       **error
){
	g_autoptr(AxisDbResult) roots = NULL;
	AxisDbConn *conn;
	gboolean ok = TRUE;
	guint i;

	if (queued != NULL)
		*queued = 0;
	if (!axis_db_pool_exec(self->pool, "SELECT ir.root_id FROM library_identity_root ir"
	        " JOIN library_root r ON r.id = ir.root_id AND r.media_type IN ('movies', 'tv')"
	        " WHERE ir.retry_after IS NULL AND (ir.pending > 0 OR ir.seasons_pending > 0)"
	        " ORDER BY ir.root_id LIMIT 1000", 0, NULL, &roots, error))
		return FALSE;
	if (axis_db_result_get_n_rows(roots) == 0)
		return TRUE;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	for (i = 0; ok && i < axis_db_result_get_n_rows(roots); i++) {
		gboolean created = FALSE;

		ok = axis_identify_enqueue(conn, axis_identify_row_int(roots, i, 0, 0), 0, &created, error);
		if (ok && created && queued != NULL)
			(*queued)++;
	}
	axis_db_pool_release(self->pool, conn);
	return ok;
}
