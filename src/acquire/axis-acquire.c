/*
 * axis-acquire.c - the acquisition layer's lifetime and settings, its small
 * helpers, the blocklist and its key, the stored searches read back, and
 * the pacing of automatic searches
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "acquire/axis-acquire-private.h"

#include <json-glib/json-glib.h>
#include <string.h>

/* ── Small helpers ──────────────────────────────────────────────── */

gchar *
axis_acquire_int_text(gint64 value)
{
	return g_strdup_printf("%" G_GINT64_FORMAT, value);
}

gchar *
axis_acquire_text_cut(
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

		if (g_unichar_iscntrl(c))
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

gboolean
axis_acquire_cause_valid(const gchar *cause)
{
	return g_strcmp0(cause, "manual") == 0 || g_strcmp0(cause, "added") == 0 || g_strcmp0(cause, "scheduled") == 0 ||
		g_strcmp0(cause, "retry") == 0 || g_strcmp0(cause, "profile") == 0 || g_strcmp0(cause, "list") == 0;
}

static const struct
{
	const gchar          *outcome;
	AxisAcquireAttention  kind;
} attention_outcomes[] = {
	{ "none_acceptable", AXIS_ACQUIRE_ATTENTION_REJECTED },
	{ "exhausted", AXIS_ACQUIRE_ATTENTION_EXHAUSTED },
	{ "no_indexer", AXIS_ACQUIRE_ATTENTION_NO_INDEXER },
	{ "indexer_error", AXIS_ACQUIRE_ATTENTION_INDEXER_ERROR },
	{ "no_client", AXIS_ACQUIRE_ATTENTION_NO_CLIENT },
	{ "client_error", AXIS_ACQUIRE_ATTENTION_CLIENT_ERROR },
	{ "root_offline", AXIS_ACQUIRE_ATTENTION_ROOT_OFFLINE },
	{ "profile_unusable", AXIS_ACQUIRE_ATTENTION_PROFILE_UNUSABLE },
	{ "metadata_missing", AXIS_ACQUIRE_ATTENTION_METADATA },
	{ "failed", AXIS_ACQUIRE_ATTENTION_FAILED }
};

AxisAcquireAttention
axis_acquire_attention_of_outcome(const gchar *outcome)
{
	guint i;

	for (i = 0; outcome != NULL && i < G_N_ELEMENTS(attention_outcomes); i++) {
		if (strcmp(attention_outcomes[i].outcome, outcome) == 0)
			return attention_outcomes[i].kind;
	}
	return AXIS_ACQUIRE_ATTENTION_NONE;
}

const gchar *
axis_acquire_attention_phrase(AxisAcquireAttention kind)
{
	static const gchar *const phrases[] = {
		"nothing", "the import needs a person", "a download is left at its client", "no destination",
		"the destination is offline", "the quality profile cannot be used", "the profile rejected what was found",
		"every acceptable release failed", "no indexer", "the indexers did not answer", "no download client",
		"the download client failed", "the metadata could not be read", "the search did not finish"
	};

	return (guint)kind < G_N_ELEMENTS(phrases) ? phrases[kind] : "something else";
}

gboolean
axis_acquire_attention_is_own(AxisAcquireAttention kind)
{
	switch (kind) {
	case AXIS_ACQUIRE_ATTENTION_LEFT_AT_CLIENT:
	case AXIS_ACQUIRE_ATTENTION_NO_ROOT:
	case AXIS_ACQUIRE_ATTENTION_PROFILE_UNUSABLE:
	case AXIS_ACQUIRE_ATTENTION_REJECTED:
	case AXIS_ACQUIRE_ATTENTION_EXHAUSTED:
	case AXIS_ACQUIRE_ATTENTION_METADATA:
		return TRUE;
	case AXIS_ACQUIRE_ATTENTION_NONE:
	case AXIS_ACQUIRE_ATTENTION_IMPORT:
	case AXIS_ACQUIRE_ATTENTION_ROOT_OFFLINE:
	case AXIS_ACQUIRE_ATTENTION_NO_INDEXER:
	case AXIS_ACQUIRE_ATTENTION_INDEXER_ERROR:
	case AXIS_ACQUIRE_ATTENTION_NO_CLIENT:
	case AXIS_ACQUIRE_ATTENTION_CLIENT_ERROR:
	case AXIS_ACQUIRE_ATTENTION_FAILED:
	default:
		return FALSE;
	}
}

const gchar *
axis_acquire_attention_because(AxisAcquireAttention kind)
{
	switch (kind) {
	case AXIS_ACQUIRE_ATTENTION_LEFT_AT_CLIENT:
		return "a download Axis gave up on is still at its download client";
	case AXIS_ACQUIRE_ATTENTION_NO_ROOT:
		return "no library folder is set as the destination";
	case AXIS_ACQUIRE_ATTENTION_PROFILE_UNUSABLE:
		return "the quality profile cannot be used";
	case AXIS_ACQUIRE_ATTENTION_REJECTED:
		return "the profile rejected what was found";
	case AXIS_ACQUIRE_ATTENTION_EXHAUSTED:
		return "every acceptable release failed";
	case AXIS_ACQUIRE_ATTENTION_METADATA:
		return "the record could not be read";
	default:
		return "of a reason its page gives";
	}
}

static gchar *
get_text(
	AxisDbResult *result,
	guint         row,
	guint         column
){
	const gchar *text = axis_db_result_get_text(result, row, column);

	return text != NULL ? g_strdup(text) : NULL;
}

static gint64
get_int(
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

static gboolean
get_bool(
	AxisDbResult *result,
	guint         row,
	guint         column
){
	gboolean value = FALSE;

	axis_db_result_get_boolean(result, row, column, &value);
	return value;
}

/* ── The blocklist key ──────────────────────────────────────────── */

gchar *
axis_acquire_name_key(
	gint64       source_id,
	gint64       indexer_id,
	const gchar *indexer,
	const gchar *name
){
	g_autofree gchar *folded = NULL;
	g_autofree gchar *indexer_folded = NULL;
	g_autoptr(GString) key = g_string_new(NULL);
	gboolean dot = FALSE;
	const gchar *p;

	g_string_append_printf(key, "%" G_GINT64_FORMAT ":", source_id);
	if (indexer_id > 0) {
		g_string_append_printf(key, "%" G_GINT64_FORMAT, indexer_id);
	} else if (indexer != NULL && g_utf8_validate(indexer, -1, NULL)) {
		indexer_folded = g_utf8_casefold(indexer, -1);
		for (p = indexer_folded; *p != '\0' && key->len < 140; p = g_utf8_next_char(p)) {
			gunichar c = g_utf8_get_char(p);

			if (g_unichar_isalnum(c))
				g_string_append_unichar(key, c);
		}
	} else {
		g_string_append_c(key, '-');
	}
	g_string_append_c(key, ':');
	if (name != NULL && g_utf8_validate(name, -1, NULL))
		folded = g_utf8_casefold(name, -1);
	for (p = folded != NULL ? folded : ""; *p != '\0'; p = g_utf8_next_char(p)) {
		gunichar c = g_utf8_get_char(p);
		gchar buffer[8];
		gint length;

		if (!g_unichar_isalnum(c)) {
			dot = TRUE;
			continue;
		}
		length = g_unichar_to_utf8(c, buffer);
		if (key->len + (gsize)length + 1 > 400)
			break;
		if (dot && key->str[key->len - 1] != ':')
			g_string_append_c(key, '.');
		dot = FALSE;
		g_string_append_len(key, buffer, length);
	}
	if (key->str[key->len - 1] == ':')
		g_string_append_c(key, '-');
	return g_string_free(g_steal_pointer(&key), FALSE);
}

/* ── Lifetime and settings ──────────────────────────────────────── */

/* A table or column name the shared statements are built from:
 * ^[a-z_][a-z0-9_]*$, so that nothing but a plain name can ever be put into
 * them. */
static gboolean
sql_name_valid(const gchar *name)
{
	const gchar *p;

	if (name == NULL || name[0] == '\0' || !(g_ascii_islower(name[0]) || name[0] == '_'))
		return FALSE;
	for (p = name; *p != '\0'; p++) {
		if (!(g_ascii_islower(*p) || g_ascii_isdigit(*p) || *p == '_'))
			return FALSE;
	}
	return TRUE;
}

static gboolean
info_valid(const AxisAcquireOwnerInfo *info)
{
	return sql_name_valid(info->module) && sql_name_valid(info->owner_table) && sql_name_valid(info->owner_column) &&
		sql_name_valid(info->search_table) && sql_name_valid(info->candidate_table) &&
		sql_name_valid(info->grab_table) && sql_name_valid(info->blocklist_table) &&
		sql_name_valid(info->pace_table);
}

AxisAcquire *
axis_acquire_new(
	AxisDbPool                 *pool,
	AxisSettings               *settings,
	AxisIndexers               *indexers,
	AxisDownloads              *downloads,
	AxisStorage                *storage,
	const AxisAcquireOwnerInfo *info,
	gpointer                    data
){
	AxisAcquire *self;

	g_return_val_if_fail(pool != NULL && indexers != NULL && downloads != NULL && storage != NULL && info != NULL &&
		info->load != NULL && info->prepare != NULL && info->claim_check != NULL && info->event != NULL &&
		info->retry != NULL, NULL);
	/* The names are put into SQL: only compile-time constants of the module's
	 * own source, checked here (the job-key expressions are string literals
	 * of the module's source too, and are not names). */
	g_assert(info_valid(info));

	self = g_new0(AxisAcquire, 1);
	self->pool = pool;
	self->settings = settings != NULL ? axis_settings_ref(settings) : NULL;
	self->indexers = axis_indexers_ref(indexers);
	self->downloads = axis_downloads_ref(downloads);
	self->storage = axis_storage_ref(storage);
	self->info = info;
	self->data = data;
	g_mutex_init(&self->hook_lock);
	return self;
}

void
axis_acquire_free(AxisAcquire *self)
{
	if (self == NULL)
		return;
	axis_indexers_unref(self->indexers);
	axis_downloads_unref(self->downloads);
	axis_storage_unref(self->storage);
	if (self->settings != NULL)
		axis_settings_unref(self->settings);
	g_mutex_clear(&self->hook_lock);
	g_free(self);
}

void
axis_acquire_set_compat(
	AxisAcquire *self,
	AxisCompat  *compat
){
	self->compat = compat;
}

const AxisAcquireOwnerInfo *
axis_acquire_get_info(AxisAcquire *self)
{
	return self->info;
}

void
axis_acquire_set_claim_hook(
	AxisAcquire          *self,
	AxisAcquireClaimHook  hook,
	gpointer              user_data
){
	g_mutex_lock(&self->hook_lock);
	self->claim_hook = hook;
	self->claim_hook_data = user_data;
	g_mutex_unlock(&self->hook_lock);
}

void
axis_acquire_set_replace_hook(
	AxisAcquire          *self,
	AxisAcquireClaimHook  hook,
	gpointer              user_data
){
	g_mutex_lock(&self->hook_lock);
	self->replace_hook = hook;
	self->replace_hook_data = user_data;
	g_mutex_unlock(&self->hook_lock);
}

static gboolean
declared(
	AxisAcquire *self,
	const gchar *key
){
	return self->settings != NULL && key != NULL && axis_settings_lookup(self->settings, key, NULL) != NULL;
}

gint64
axis_acquire_setting_duration(
	AxisAcquire *self,
	const gchar *key,
	gint64       fallback
){
	return declared(self, key) ? axis_settings_get_duration(self->settings, key) : fallback;
}

gint64
axis_acquire_setting_integer(
	AxisAcquire *self,
	const gchar *key,
	gint64       fallback
){
	return declared(self, key) ? axis_settings_get_integer(self->settings, key) : fallback;
}

gchar *
axis_acquire_setting_string(
	AxisAcquire *self,
	const gchar *key,
	const gchar *fallback
){
	return declared(self, key) ? axis_settings_get_string(self->settings, key) : g_strdup(fallback);
}

gchar **
axis_acquire_setting_languages(AxisAcquire *self)
{
	const gchar *key = self->info->setting_languages;
	g_auto(GStrv) items = declared(self, key) ? axis_settings_get_string_list(self->settings, key) : NULL;
	GPtrArray *codes = g_ptr_array_new();
	guint i;

	for (i = 0; items != NULL && items[i] != NULL; i++) {
		g_autofree gchar *code = g_ascii_strdown(g_strstrip(items[i]), -1);

		if (strlen(code) == 2 && g_ascii_isalpha(code[0]) && g_ascii_isalpha(code[1]))
			g_ptr_array_add(codes, g_steal_pointer(&code));
	}
	g_ptr_array_add(codes, NULL);
	return (gchar **)g_ptr_array_free(codes, FALSE);
}

/* ── The module's statements and history ────────────────────────── */

gchar *
axis_acquire_sql(
	AxisAcquire *self,
	const gchar *pattern
){
	const AxisAcquireOwnerInfo *info = self->info;
	GString *sql = g_string_sized_new(strlen(pattern) + 64);
	const gchar *p;

	for (p = pattern; *p != '\0'; p++) {
		const gchar *with = NULL;

		if (*p != '%' || p[1] == '\0') {
			g_string_append_c(sql, *p);
			continue;
		}
		p++;
		switch (*p) {
		case 'T': with = info->owner_table; break;
		case 'O': with = info->owner_column; break;
		case 'S': with = info->search_table; break;
		case 'C': with = info->candidate_table; break;
		case 'G': with = info->grab_table; break;
		case 'B': with = info->blocklist_table; break;
		case 'P': with = info->pace_table; break;
		case 'J': with = info->search_job_sql != NULL ? info->search_job_sql : "NULL::text"; break;
		case 'K': with = info->grab_job_sql != NULL ? info->grab_job_sql : "NULL::text"; break;
		case 'M': with = info->module; break;
		case '%': with = "%"; break;
		default:
			g_string_append_c(sql, '%');
			g_string_append_c(sql, *p);
			continue;
		}
		g_string_append(sql, with);
	}
	return g_string_free(sql, FALSE);
}

gboolean
axis_acquire_eventf(
	AxisAcquire            *self,
	AxisDbConn             *conn,
	gint64                  owner_id,
	const AxisAcquireWant  *want,
	const gchar            *kind,
	const gchar            *user,
	gint64                  search_id,
	gint64                  grab_id,
	GError                **error,
	const gchar            *format,
	...
){
	g_autofree gchar *text = NULL;
	va_list ap;

	va_start(ap, format);
	text = g_strdup_vprintf(format, ap);
	va_end(ap);
	return self->info->event(self->data, conn, owner_id, want, kind, user, search_id, grab_id, text, error);
}

/* ── What is wanted ─────────────────────────────────────────────── */

AxisAcquireWant *
axis_acquire_want_new(void)
{
	AxisAcquireWant *want = g_new0(AxisAcquireWant, 1);

	want->pace_ids = g_array_new(FALSE, FALSE, sizeof(gint64));
	return want;
}

void
axis_acquire_want_free(AxisAcquireWant *want)
{
	if (want == NULL)
		return;
	g_free(want->label);
	g_free(want->profile_name);
	g_free(want->root_name);
	g_free(want->volume_name);
	g_clear_pointer(&want->pace_ids, g_array_unref);
	axis_release_free(want->disk_fields);
	g_free(want->disk_name);
	g_clear_pointer(&want->asks, g_ptr_array_unref);
	g_free(want->ask_label);
	if (want->data_free != NULL && want->data != NULL)
		want->data_free(want->data);
	g_free(want);
}

static void
ask_free(AxisAcquireAsk *ask)
{
	g_free(ask->words);
	g_free(ask->label);
	g_free(ask);
}

void
axis_acquire_want_add_ask(
	AxisAcquireWant          *want,
	const AxisIndexerRequest *request,
	const gchar              *words,
	const gchar              *label
){
	AxisAcquireAsk *ask;
	guint i;

	g_return_if_fail(want != NULL && request != NULL && words != NULL && label != NULL);

	if (want->asks == NULL)
		want->asks = g_ptr_array_new_with_free_func((GDestroyNotify)ask_free);
	/* The same words twice are asked once. */
	for (i = 0; i < want->asks->len; i++) {
		const AxisAcquireAsk *have = g_ptr_array_index(want->asks, i);
		g_autofree gchar *a = g_utf8_casefold(have->words, -1);
		g_autofree gchar *b = g_utf8_casefold(words, -1);

		if (have->request.kind == request->kind && strcmp(a, b) == 0)
			return;
	}
	if (want->asks->len >= AXIS_ACQUIRE_MAX_ASKS)
		return;
	ask = g_new0(AxisAcquireAsk, 1);
	ask->request = *request;
	ask->words = g_strdup(words);
	ask->request.title = ask->words;
	ask->label = g_strdup(label);
	g_ptr_array_add(want->asks, ask);
}

void
axis_acquire_disk_set(
	AxisQualityOnDisk      *disk,
	AxisRelease           **fields,
	gchar                 **name_out,
	AxisQuality             quality,
	AxisReleaseResolution   resolution,
	const gchar            *name
){
	*name_out = name != NULL ? g_strdup(name) : NULL;
	*fields = *name_out != NULL ? axis_release_parse(*name_out) : NULL;
	memset(disk, 0, sizeof(*disk));
	disk->quality = quality;
	disk->resolution = resolution;
	disk->fields = *fields;
	disk->name = *name_out;
	disk->converted = FALSE;
	disk->source_quality = AXIS_QUALITY_UNKNOWN;
}

/* ── Reasons ────────────────────────────────────────────────────── */

AxisAcquireReason *
axis_acquire_reason_new(
	const gchar *kind,
	const gchar *code,
	const gchar *text
){
	AxisAcquireReason *reason = g_new0(AxisAcquireReason, 1);

	reason->kind = g_strdup(kind);
	reason->code = g_strdup(code);
	reason->text = axis_acquire_text_cut(text, 600);
	return reason;
}

void
axis_acquire_reason_free(AxisAcquireReason *reason)
{
	if (reason == NULL)
		return;
	g_free(reason->kind);
	g_free(reason->code);
	g_free(reason->text);
	g_free(reason);
}

gchar *
axis_acquire_reasons_to_json(const GPtrArray *reasons)
{
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonGenerator) generator = json_generator_new();
	g_autoptr(JsonNode) root = NULL;
	guint i;

	json_builder_begin_array(builder);
	for (i = 0; reasons != NULL && i < reasons->len; i++) {
		const AxisAcquireReason *reason = g_ptr_array_index(reasons, i);

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "k");
		json_builder_add_string_value(builder, reason->kind != NULL ? reason->kind : "profile");
		json_builder_set_member_name(builder, "c");
		json_builder_add_string_value(builder, reason->code != NULL ? reason->code : "");
		json_builder_set_member_name(builder, "t");
		json_builder_add_string_value(builder, reason->text != NULL ? reason->text : "");
		json_builder_end_object(builder);
	}
	json_builder_end_array(builder);
	root = json_builder_get_root(builder);
	json_generator_set_root(generator, root);
	return json_generator_to_data(generator, NULL);
}

static const gchar *
member_text(
	JsonObject  *object,
	const gchar *name
){
	JsonNode *node = object != NULL ? json_object_get_member(object, name) : NULL;

	if (node == NULL || !JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_STRING)
		return NULL;
	return json_node_get_string(node);
}

GPtrArray *
axis_acquire_reasons_from_json(const gchar *json)
{
	GPtrArray *reasons = g_ptr_array_new_with_free_func((GDestroyNotify)axis_acquire_reason_free);
	g_autoptr(JsonParser) parser = json_parser_new();
	JsonNode *root;
	JsonArray *array;
	guint i;

	if (json == NULL || !json_parser_load_from_data(parser, json, -1, NULL))
		return reasons;
	root = json_parser_get_root(parser);
	if (root == NULL || !JSON_NODE_HOLDS_ARRAY(root))
		return reasons;
	array = json_node_get_array(root);
	for (i = 0; i < json_array_get_length(array) && i < 64; i++) {
		JsonNode *node = json_array_get_element(array, i);
		JsonObject *object = JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
		const gchar *text = member_text(object, "t");

		if (text == NULL)
			continue;
		g_ptr_array_add(reasons, axis_acquire_reason_new(member_text(object, "k"), member_text(object, "c"), text));
	}
	return reasons;
}

/* ── Stored rows ────────────────────────────────────────────────── */

void
axis_acquire_candidate_free(AxisAcquireCandidate *candidate)
{
	if (candidate == NULL)
		return;
	g_free(candidate->name);
	g_free(candidate->indexer);
	g_free(candidate->info_hash);
	g_free(candidate->name_key);
	g_free(candidate->quality);
	g_free(candidate->match_text);
	g_clear_pointer(&candidate->reasons, g_ptr_array_unref);
	g_free(candidate->note);
	g_free(candidate->blocked_by);
	g_free(candidate);
}

AxisAcquireCandidate *
axis_acquire_candidate_from_row(
	AxisDbResult *result,
	guint         row
){
	AxisAcquireCandidate *c = g_new0(AxisAcquireCandidate, 1);
	const gchar *match = axis_db_result_get_text(result, row, 15);

	c->id = get_int(result, row, 0, 0);
	c->search_id = get_int(result, row, 1, 0);
	c->position = (gint)get_int(result, row, 2, 0);
	c->release_id = get_int(result, row, 3, 0);
	c->source_id = get_int(result, row, 4, 0);
	c->name = get_text(result, row, 5);
	axis_download_protocol_from_string(axis_db_result_get_text(result, row, 6), &c->protocol);
	c->size_bytes = get_int(result, row, 7, -1);
	c->published_us = get_int(result, row, 8, 0);
	c->seeders = (gint)get_int(result, row, 9, -1);
	c->indexer = get_text(result, row, 10);
	c->indexer_id = get_int(result, row, 11, 0);
	c->info_hash = get_text(result, row, 12);
	c->name_key = get_text(result, row, 13);
	c->quality = get_text(result, row, 14);
	c->match = g_strcmp0(match, "match") == 0 ? AXIS_RELEASE_MATCH_YES :
		g_strcmp0(match, "ambiguous") == 0 ? AXIS_RELEASE_MATCH_AMBIGUOUS : AXIS_RELEASE_MATCH_NONE;
	c->match_text = get_text(result, row, 16);
	c->accepted = get_bool(result, row, 17);
	c->safety = get_bool(result, row, 18);
	c->blocklisted = get_bool(result, row, 19);
	c->rank = (guint)get_int(result, row, 20, 0);
	c->score = (gint)get_int(result, row, 21, 0);
	c->reasons = axis_acquire_reasons_from_json(axis_db_result_get_text(result, row, 22));
	c->note = get_text(result, row, 23);
	return c;
}

void
axis_acquire_block_free(AxisAcquireBlock *block)
{
	if (block == NULL)
		return;
	g_free(block->release_name);
	g_free(block->indexer);
	g_free(block->reason);
	g_free(block->reason_text);
	g_free(block->created_by);
	g_free(block->info_hash);
	g_free(block);
}

void
axis_acquire_search_info_free(AxisAcquireSearchInfo *search)
{
	if (search == NULL)
		return;
	g_free(search->cause);
	g_free(search->requested_by);
	g_free(search->outcome);
	g_free(search->outcome_text);
	g_free(search->how);
	g_free(search);
}

gboolean
axis_acquire_store_lock(
	AxisAcquire  *self,
	AxisDbConn   *conn,
	gint64        owner_id,
	gboolean     *found,
	GError      **error
){
	g_autofree gchar *id = axis_acquire_int_text(owner_id);
	g_autofree gchar *sql = axis_acquire_sql(self, "SELECT id FROM %T WHERE id = $1::bigint FOR UPDATE");
	g_autoptr(AxisDbResult) result = NULL;
	const gchar *params[1];

	params[0] = id;
	if (!axis_db_conn_exec(conn, sql, 1, params, &result, error))
		return FALSE;
	*found = axis_db_result_get_n_rows(result) > 0;
	return TRUE;
}

gboolean
axis_acquire_store_search(
	AxisAcquire            *self,
	AxisDbConn             *conn,
	gint64                  owner_id,
	gint64                  search_id,
	AxisAcquireSearchInfo **search_out,
	GError                **error
){
	g_autofree gchar *owner = axis_acquire_int_text(owner_id);
	g_autofree gchar *id = axis_acquire_int_text(search_id);
	/* The owner's latest is its latest automatic search: a manual search
	 * (cause "list") is a person's look, read by its id. */
	g_autofree gchar *sql = axis_acquire_sql(self, "SELECT id, cause, requested_by, " AXIS_ACQUIRE_US("started_at")
		", " AXIS_ACQUIRE_US("finished_at") ", outcome, outcome_text, how, n_sources, n_found, n_accepted, n_compared"
		" FROM %S WHERE %O = $1::bigint AND (($2::bigint = 0 AND cause <> 'list') OR id = $2::bigint)"
		" ORDER BY id DESC LIMIT 1");
	g_autoptr(AxisDbResult) rows = NULL;
	AxisAcquireSearchInfo *info;
	const gchar *params[2];

	*search_out = NULL;
	params[0] = owner;
	params[1] = id;
	if (!axis_db_conn_exec(conn, sql, 2, params, &rows, error))
		return FALSE;
	if (axis_db_result_get_n_rows(rows) == 0)
		return TRUE;
	info = g_new0(AxisAcquireSearchInfo, 1);
	info->id = get_int(rows, 0, 0, 0);
	info->cause = get_text(rows, 0, 1);
	info->requested_by = get_text(rows, 0, 2);
	info->started_us = get_int(rows, 0, 3, 0);
	info->finished_us = get_int(rows, 0, 4, 0);
	info->outcome = get_text(rows, 0, 5);
	info->outcome_text = get_text(rows, 0, 6);
	info->how = get_text(rows, 0, 7);
	info->n_sources = (guint)get_int(rows, 0, 8, 0);
	info->n_found = (guint)get_int(rows, 0, 9, 0);
	info->n_accepted = (guint)get_int(rows, 0, 10, 0);
	info->n_compared = (guint)get_int(rows, 0, 11, 0);
	*search_out = info;
	return TRUE;
}

gboolean
axis_acquire_store_candidates(
	AxisAcquire  *self,
	AxisDbConn   *conn,
	gint64        search_id,
	GPtrArray   **candidates,
	GError      **error
){
	g_autofree gchar *id = axis_acquire_int_text(search_id);
	g_autofree gchar *sql = axis_acquire_sql(self, "SELECT " AXIS_ACQUIRE_CANDIDATE_COLUMNS " FROM %C c"
		" WHERE c.search_id = $1::bigint ORDER BY c.position");
	g_autoptr(AxisDbResult) rows = NULL;
	const gchar *params[1];
	guint i;

	params[0] = id;
	if (!axis_db_conn_exec(conn, sql, 1, params, &rows, error))
		return FALSE;
	*candidates = g_ptr_array_new_with_free_func((GDestroyNotify)axis_acquire_candidate_free);
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++)
		g_ptr_array_add(*candidates, axis_acquire_candidate_from_row(rows, i));
	return TRUE;
}

gboolean
axis_acquire_store_candidate(
	AxisAcquire           *self,
	AxisDbConn            *conn,
	gint64                 owner_id,
	gint64                 candidate_id,
	AxisAcquireCandidate **candidate,
	GError               **error
){
	g_autofree gchar *owner = axis_acquire_int_text(owner_id);
	g_autofree gchar *id = axis_acquire_int_text(candidate_id);
	g_autofree gchar *sql = axis_acquire_sql(self, "SELECT " AXIS_ACQUIRE_CANDIDATE_COLUMNS " FROM %C c"
		" JOIN %S s ON s.id = c.search_id WHERE c.id = $1::bigint AND s.%O = $2::bigint");
	g_autoptr(AxisDbResult) result = NULL;
	const gchar *params[2];

	*candidate = NULL;
	params[0] = id;
	params[1] = owner;
	if (!axis_db_conn_exec(conn, sql, 2, params, &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) > 0)
		*candidate = axis_acquire_candidate_from_row(result, 0);
	return TRUE;
}

/* ── The blocklist ──────────────────────────────────────────────── */

void
axis_acquire_block_key_free(AxisAcquireBlockKey *key)
{
	if (key == NULL)
		return;
	g_free(key->key);
	g_free(key->name_key);
	g_free(key->info_hash);
	g_free(key->reason);
	g_free(key->created_by);
	g_free(key);
}

gboolean
axis_acquire_store_block_keys(
	AxisAcquire  *self,
	AxisDbConn   *conn,
	gint64        owner_id,
	GPtrArray   **keys,
	GError      **error
){
	g_autofree gchar *id = axis_acquire_int_text(owner_id);
	g_autofree gchar *sql = axis_acquire_sql(self, "SELECT key, name_key, info_hash, reason, created_by FROM %B"
		" WHERE %O = $1::bigint ORDER BY id");
	g_autoptr(AxisDbResult) result = NULL;
	const gchar *params[1];
	guint i;

	params[0] = id;
	if (!axis_db_conn_exec(conn, sql, 1, params, &result, error))
		return FALSE;
	*keys = g_ptr_array_new_with_free_func((GDestroyNotify)axis_acquire_block_key_free);
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		AxisAcquireBlockKey *key = g_new0(AxisAcquireBlockKey, 1);

		key->key = get_text(result, i, 0);
		key->name_key = get_text(result, i, 1);
		key->info_hash = get_text(result, i, 2);
		key->reason = get_text(result, i, 3);
		key->created_by = get_text(result, i, 4);
		g_ptr_array_add(*keys, key);
	}
	return TRUE;
}

/* The folded release name of a name key ("<source>:<indexer>:<name>": what
 * follows its second colon; neither the source's number nor the indexer's
 * folded name holds one), %NULL when it has none. */
static const gchar *
name_of_key(const gchar *name_key)
{
	const gchar *colon = name_key != NULL ? strchr(name_key, ':') : NULL;

	colon = colon != NULL ? strchr(colon + 1, ':') : NULL;
	return colon != NULL && colon[1] != '\0' ? colon + 1 : NULL;
}

const AxisAcquireBlockKey *
axis_acquire_blocked_by(
	const GPtrArray *keys,
	const gchar     *name_key,
	const gchar     *info_hash
){
	const AxisAcquireBlockKey *found = NULL;
	const gchar *name = name_of_key(name_key);
	guint i;

	for (i = 0; keys != NULL && i < keys->len; i++) {
		const AxisAcquireBlockKey *key = g_ptr_array_index(keys, i);
		gboolean person = g_strcmp0(key->reason, AXIS_ACQUIRE_BLOCK_PERSON) == 0;
		gboolean on = FALSE;

		if (info_hash != NULL && key->info_hash != NULL && strcmp(info_hash, key->info_hash) == 0)
			on = TRUE;
		else if (name_key != NULL && key->name_key != NULL && strcmp(name_key, key->name_key) == 0)
			on = TRUE;
		/* A person blocked the release: the same name from any indexer. */
		else if (person && name != NULL && g_strcmp0(name, name_of_key(key->name_key)) == 0)
			on = TRUE;
		if (!on)
			continue;
		if (person)
			return key;
		if (found == NULL)
			found = key;
	}
	return found;
}

gboolean
axis_acquire_blocked(
	const GPtrArray *keys,
	const gchar     *name_key,
	const gchar     *info_hash
){
	return axis_acquire_blocked_by(keys, name_key, info_hash) != NULL;
}

gboolean
axis_acquire_block_by_person(const gchar *reason)
{
	return g_strcmp0(reason, AXIS_ACQUIRE_BLOCK_PERSON) == 0 || g_strcmp0(reason, "given_up") == 0;
}

gchar *
axis_acquire_block_words(
	const gchar *reason,
	const gchar *created_by
){
	if (g_strcmp0(reason, AXIS_ACQUIRE_BLOCK_PERSON) == 0)
		return g_strdup_printf("blocked by %s", created_by != NULL ? created_by : "an administrator");
	if (g_strcmp0(reason, "given_up") == 0)
		return g_strdup_printf("%s gave up on its download", created_by != NULL ? created_by : "an administrator");
	return g_strdup("it failed before");
}

gboolean
axis_acquire_store_block(
	AxisAcquire  *self,
	AxisDbConn   *conn,
	gint64        owner_id,
	const gchar  *name_key,
	const gchar  *info_hash,
	const gchar  *release_name,
	const gchar  *indexer,
	const gchar  *reason,
	const gchar  *reason_text,
	gint64        grab_id,
	const gchar  *user,
	GError      **error
){
	return axis_acquire_store_block_written(self, conn, owner_id, name_key, info_hash, release_name, indexer, reason,
		reason_text, grab_id, user, NULL, error);
}

gboolean
axis_acquire_store_block_written(
	AxisAcquire  *self,
	AxisDbConn   *conn,
	gint64        owner_id,
	const gchar  *name_key,
	const gchar  *info_hash,
	const gchar  *release_name,
	const gchar  *indexer,
	const gchar  *reason,
	const gchar  *reason_text,
	gint64        grab_id,
	const gchar  *user,
	gboolean     *written,
	GError      **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *owner = axis_acquire_int_text(owner_id);
	g_autofree gchar *grab = grab_id > 0 ? axis_acquire_int_text(grab_id) : NULL;
	g_autofree gchar *hash = info_hash != NULL ? g_ascii_strdown(info_hash, -1) : NULL;
	g_autofree gchar *key = NULL;
	g_autofree gchar *name = axis_acquire_text_cut(release_name, 300);
	g_autofree gchar *why = axis_acquire_text_cut(reason_text, 1000);
	g_autofree gchar *by = axis_acquire_text_cut(user, 200);
	/* Once; but a person's block takes the place of a failure's entry of the
	 * same key: it says who, and it is matched from any indexer. */
	g_autofree gchar *sql = axis_acquire_sql(self, g_strcmp0(reason, AXIS_ACQUIRE_BLOCK_PERSON) == 0 ?
		"INSERT INTO %B (%O, key, name_key, info_hash, release_name,"
		" indexer, reason, reason_text, grab_id, created_by) VALUES ($1::bigint, $2, $3, $4, $5, $6, $7, $8,"
		" $9::bigint, $10) ON CONFLICT (%O, key) DO UPDATE SET reason = EXCLUDED.reason,"
		" reason_text = CASE WHEN %B.reason_text IS NULL THEN EXCLUDED.reason_text"
		" ELSE left(EXCLUDED.reason_text || ' Before: ' || %B.reason_text, 1000) END,"
		" created_by = EXCLUDED.created_by, created_at = now(),"
		" grab_id = coalesce(EXCLUDED.grab_id, %B.grab_id) WHERE %B.reason <> EXCLUDED.reason RETURNING 1" :
		"INSERT INTO %B (%O, key, name_key, info_hash, release_name,"
		" indexer, reason, reason_text, grab_id, created_by) VALUES ($1::bigint, $2, $3, $4, $5, $6, $7, $8,"
		" $9::bigint, $10) ON CONFLICT (%O, key) DO NOTHING RETURNING 1");
	const gchar *params[10];

	if (hash != NULL && !axis_download_hash_valid(hash))
		g_clear_pointer(&hash, g_free);
	key = hash != NULL ? g_strdup_printf("hash:%s", hash) : g_strdup_printf("name:%s", name_key);
	params[0] = owner;
	params[1] = key;
	params[2] = name_key;
	params[3] = hash;
	params[4] = name != NULL ? name : name_key;
	params[5] = indexer;
	params[6] = reason;
	params[7] = why;
	params[8] = grab;
	params[9] = by;
	if (!axis_db_conn_exec(conn, sql, 10, params, &result, error))
		return FALSE;
	if (written != NULL)
		*written = axis_db_result_get_n_rows(result) > 0;
	return TRUE;
}

gboolean
axis_acquire_store_unblock(
	AxisAcquire  *self,
	AxisDbConn   *conn,
	gint64        owner_id,
	gint64        entry_id,
	GPtrArray   **names,
	GError      **error
){
	g_autofree gchar *owner = axis_acquire_int_text(owner_id);
	g_autofree gchar *entry = axis_acquire_int_text(entry_id);
	g_autofree gchar *sql = axis_acquire_sql(self, entry_id > 0 ?
		"DELETE FROM %B WHERE %O = $1::bigint AND id = $2::bigint RETURNING release_name" :
		"DELETE FROM %B WHERE %O = $1::bigint AND $2::bigint = 0 RETURNING release_name");
	g_autoptr(AxisDbResult) deleted = NULL;
	const gchar *params[2];
	guint i;

	params[0] = owner;
	params[1] = entry;
	if (!axis_db_conn_exec(conn, sql, 2, params, &deleted, error))
		return FALSE;
	*names = g_ptr_array_new_with_free_func(g_free);
	for (i = 0; i < axis_db_result_get_n_rows(deleted); i++)
		g_ptr_array_add(*names, get_text(deleted, i, 0));
	return TRUE;
}

gboolean
axis_acquire_store_blocklist(
	AxisAcquire  *self,
	AxisDbConn   *conn,
	gint64        owner_id,
	GPtrArray   **blocks,
	GError      **error
){
	g_autofree gchar *owner = axis_acquire_int_text(owner_id);
	g_autofree gchar *sql = axis_acquire_sql(self, "SELECT id, release_name, indexer, reason, reason_text, created_by, "
		AXIS_ACQUIRE_US("created_at") ", info_hash FROM %B WHERE %O = $1::bigint ORDER BY id DESC");
	g_autoptr(AxisDbResult) rows = NULL;
	const gchar *params[1];
	guint i;

	params[0] = owner;
	if (!axis_db_conn_exec(conn, sql, 1, params, &rows, error))
		return FALSE;
	*blocks = g_ptr_array_new_with_free_func((GDestroyNotify)axis_acquire_block_free);
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		AxisAcquireBlock *block = g_new0(AxisAcquireBlock, 1);

		block->id = get_int(rows, i, 0, 0);
		block->release_name = get_text(rows, i, 1);
		block->indexer = get_text(rows, i, 2);
		block->reason = get_text(rows, i, 3);
		block->reason_text = get_text(rows, i, 4);
		block->created_by = get_text(rows, i, 5);
		block->created_us = get_int(rows, i, 6, 0);
		block->info_hash = get_text(rows, i, 7);
		g_ptr_array_add(*blocks, block);
	}
	return TRUE;
}

/* ── Pacing ─────────────────────────────────────────────────────── */

static gchar *
id_array(GArray *ids)
{
	GString *array = g_string_new("{");
	guint i;

	for (i = 0; ids != NULL && i < ids->len; i++)
		g_string_append_printf(array, "%s%" G_GINT64_FORMAT, i > 0 ? "," : "", g_array_index(ids, gint64, i));
	g_string_append_c(array, '}');
	return g_string_free(array, FALSE);
}

gboolean
axis_acquire_pace(
	AxisAcquire  *self,
	AxisDbConn   *conn,
	GArray       *ids,
	const gchar  *outcome,
	GError      **error
){
	g_autofree gchar *rows = id_array(ids);
	g_autofree gchar *base = axis_acquire_int_text(axis_acquire_setting_duration(self, self->info->setting_floor,
		6 * 3600));
	g_autofree gchar *most = axis_acquire_int_text(axis_acquire_setting_duration(self, self->info->setting_floor_max,
		7 * 86400));
	g_autofree gchar *sql = NULL;
	const gchar *params[3];

	params[0] = rows;
	params[1] = base;
	params[2] = most;
	if (ids == NULL || ids->len == 0 || g_strcmp0(outcome, "in_flight") == 0 || g_strcmp0(outcome, "failed") == 0)
		return TRUE;
	if (g_strcmp0(outcome, "grabbed") == 0) {
		sql = axis_acquire_sql(self, "UPDATE %P SET last_search_at = now(), search_floor_s = $2::integer,"
			" next_search_at = now() + $2::integer * interval '1 second' WHERE id = ANY ($1::bigint[])");
		return axis_db_conn_exec(conn, sql, 2, params, NULL, error);
	}
	/* Nothing grabbed: wait the floor, and double it while nothing is found
	 * or nothing is acceptable. */
	if (g_strcmp0(outcome, "nothing_found") == 0 || g_strcmp0(outcome, "none_acceptable") == 0 ||
	    g_strcmp0(outcome, "exhausted") == 0) {
		sql = axis_acquire_sql(self, "UPDATE %P SET last_search_at = now(),"
			" next_search_at = now() + least(greatest(search_floor_s, $2::integer), $3::integer) * interval '1 second',"
			" search_floor_s = least(greatest(search_floor_s, $2::integer) * 2, $3::integer)"
			" WHERE id = ANY ($1::bigint[])");
		return axis_db_conn_exec(conn, sql, 3, params, NULL, error);
	}
	/* Nothing could be asked (no client, no indexer, ...): wait the floor,
	 * without doubling it. */
	sql = axis_acquire_sql(self, "UPDATE %P SET last_search_at = now(),"
		" next_search_at = now() + least(greatest(search_floor_s, $2::integer), $3::integer) * interval '1 second'"
		" WHERE id = ANY ($1::bigint[])");
	return axis_db_conn_exec(conn, sql, 3, params, NULL, error);
}

/* ── Clients ────────────────────────────────────────────────────── */

gboolean
axis_acquire_read_clients(
	AxisAcquire  *self,
	gboolean     *usenet,
	gboolean     *torrent,
	GError      **error
){
	g_autoptr(GPtrArray) clients = NULL;
	guint i;

	*usenet = FALSE;
	*torrent = FALSE;
	if (!axis_downloads_list_clients(self->downloads, &clients, error))
		return FALSE;
	for (i = 0; i < clients->len; i++) {
		const AxisDownloadClientInfo *client = g_ptr_array_index(clients, i);
		const AxisDownloadKindInfo *kind = axis_downloads_find_kind(self->downloads, client->kind);

		if (!client->enabled || kind == NULL || client->state == AXIS_DOWNLOAD_CLIENT_REFUSED)
			continue;
		if (kind->protocol == AXIS_DOWNLOAD_USENET)
			*usenet = TRUE;
		else
			*torrent = TRUE;
	}
	return TRUE;
}
