/*
 * axis-worker-proto.c - the worker API's messages, read strictly and
 * written exactly, by both sides
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "api/axis-worker-proto.h"

#include <math.h>
#include <string.h>

#include "library/axis-library-name.h"
#include "probe/axis-probe-json.h"
#include "storage/axis-storage-open.h"
#include "storage/axis-storage-path.h"
#include "storage/axis-volume-marker.h"
#include "transcode/axis-sample-args.h"

/* ── Texts ──────────────────────────────────────────────────────── */

gboolean
axis_worker_name_is_valid(const gchar *name)
{
	const gchar *p;

	if (name == NULL || name[0] < 'a' || name[0] > 'z' || strlen(name) > AXIS_WORKER_NAME_MAX)
		return FALSE;
	for (p = name; *p != '\0'; p++) {
		if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-'))
			return FALSE;
	}
	return TRUE;
}

gboolean
axis_worker_kind_is_valid(const gchar *kind)
{
	const gchar *p;

	if (kind == NULL || kind[0] < 'a' || kind[0] > 'z' || strlen(kind) > AXIS_WORKER_KIND_MAX)
		return FALSE;
	for (p = kind; *p != '\0'; p++) {
		if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '.' || *p == '_' || *p == '-'))
			return FALSE;
	}
	return TRUE;
}

gboolean
axis_worker_version_is_valid(
	const gchar *version,
	gsize        max
){
	const gchar *p;
	gsize length;

	if (version == NULL)
		return FALSE;
	length = strlen(version);
	if (length == 0 || length > max)
		return FALSE;
	for (p = version; *p != '\0'; p++) {
		if (!(g_ascii_isalnum(*p) || *p == '.' || *p == '+' || *p == '~' || *p == '_' || *p == '-'))
			return FALSE;
	}
	return TRUE;
}

gboolean
axis_worker_digest_is_valid(const gchar *digest)
{
	guint i;

	if (digest == NULL || strlen(digest) != 64)
		return FALSE;
	for (i = 0; i < 64; i++) {
		if (!((digest[i] >= '0' && digest[i] <= '9') || (digest[i] >= 'a' && digest[i] <= 'f')))
			return FALSE;
	}
	return TRUE;
}

gboolean
axis_worker_uuid_is_valid(const gchar *uuid)
{
	return axis_volume_uuid_is_valid(uuid);
}

gboolean
axis_sample_skip_reason_is_valid(const gchar *reason)
{
	static const gchar *const words[] = {
		AXIS_SAMPLE_SKIP_CHANGED, AXIS_SAMPLE_SKIP_MISSING, AXIS_SAMPLE_SKIP_NOT_CONTAINED,
		AXIS_SAMPLE_SKIP_UNREADABLE, AXIS_SAMPLE_SKIP_ENCODER_FAILED, AXIS_SAMPLE_SKIP_VOLUME,
		AXIS_SAMPLE_SKIP_PATH_REFUSED, NULL
	};
	guint i;

	for (i = 0; reason != NULL && words[i] != NULL; i++) {
		if (strcmp(reason, words[i]) == 0)
			return TRUE;
	}
	return FALSE;
}

/* ── Writing ────────────────────────────────────────────────────── */

typedef struct
{
	GString  *out;
	gboolean  first[AXIS_JSON_MAX_DEPTH + 2];
	guint     depth;
	gboolean  after_key;
} Writer;

static void
w_init(Writer *w)
{
	memset(w, 0, sizeof(*w));
	w->out = g_string_new(NULL);
}

static gchar *
w_finish(Writer *w)
{
	return g_string_free(w->out, FALSE);
}

static void
w_value(Writer *w)
{
	if (w->after_key) {
		w->after_key = FALSE;
		return;
	}
	if (w->depth > 0) {
		if (!w->first[w->depth])
			g_string_append_c(w->out, ',');
		w->first[w->depth] = FALSE;
	}
}

static void
w_key(
	Writer      *w,
	const gchar *key
){
	if (!w->first[w->depth])
		g_string_append_c(w->out, ',');
	w->first[w->depth] = FALSE;
	axis_library_json_append_string(w->out, key);
	g_string_append_c(w->out, ':');
	w->after_key = TRUE;
}

static void
w_open(
	Writer *w,
	gchar   c
){
	w_value(w);
	g_string_append_c(w->out, c);
	w->depth++;
	w->first[w->depth] = TRUE;
}

static void
w_close(
	Writer *w,
	gchar   c
){
	w->depth--;
	g_string_append_c(w->out, c);
}

static void
w_string(
	Writer      *w,
	const gchar *text
){
	w_value(w);
	if (text == NULL) {
		g_string_append(w->out, "null");
	} else if (g_utf8_validate(text, -1, NULL)) {
		axis_library_json_append_string(w->out, text);
	} else {
		g_autofree gchar *valid = g_utf8_make_valid(text, -1);

		axis_library_json_append_string(w->out, valid);
	}
}

static void
w_int(
	Writer *w,
	gint64  value
){
	w_value(w);
	g_string_append_printf(w->out, "%" G_GINT64_FORMAT, value);
}

static void
w_uint(
	Writer  *w,
	guint64  value
){
	w_value(w);
	g_string_append_printf(w->out, "%" G_GUINT64_FORMAT, value);
}

static void
w_bool(
	Writer   *w,
	gboolean  value
){
	w_value(w);
	g_string_append(w->out, value ? "true" : "false");
}

static void
w_null(Writer *w)
{
	w_value(w);
	g_string_append(w->out, "null");
}

static void
w_double(
	Writer  *w,
	gdouble  value
){
	gchar buffer[G_ASCII_DTOSTR_BUF_SIZE];

	w_value(w);
	if (!isfinite(value))
		value = 0;
	g_ascii_formatd(buffer, sizeof(buffer), "%.6f", value);
	g_string_append(w->out, buffer);
}

static void
w_begin_version(
	Writer *w,
	guint   version
){
	w_init(w);
	w_open(w, '{');
	w_key(w, "protocol");
	w_int(w, version);
}

static void
w_begin_message(Writer *w)
{
	w_begin_version(w, AXIS_WORKER_PROTOCOL);
}

/* ── Reading ────────────────────────────────────────────────────── */

static gboolean
refuse(
	gchar       **problem,
	const gchar  *text
){
	if (problem != NULL && *problem == NULL)
		*problem = g_strdup(text);
	return FALSE;
}

static gboolean
refuse_member(
	gchar       **problem,
	const gchar  *name,
	const gchar  *what
){
	if (problem != NULL && *problem == NULL)
		*problem = g_strdup_printf("member \"%s\" %s", name, what);
	return FALSE;
}

static AxisJsonDoc *
read_doc(
	const gchar  *data,
	gsize         length,
	gsize         max,
	gchar       **problem
){
	g_autofree gchar *why = NULL;
	AxisJsonDoc *doc;

	if (data == NULL || length == 0) {
		refuse(problem, "the message is empty");
		return NULL;
	}
	if (length > max) {
		refuse(problem, "the message is larger than allowed");
		return NULL;
	}
	doc = axis_json_parse(data, length, &why);
	if (doc == NULL) {
		if (problem != NULL && *problem == NULL)
			*problem = g_strdup_printf("the message is not JSON within the limits: %s",
				why != NULL ? why : "malformed");
		return NULL;
	}
	return doc;
}

/* Whether @object is an object with no member outside @allowed and none
 * twice.  An object with more members than @allowed names has an unknown
 * or a repeated one, so the pairwise check only ever sees a few. */
static gboolean
only_members(
	const AxisJsonNode *object,
	const gchar        *what,
	const gchar *const *allowed,
	gchar             **problem
){
	guint n_allowed = g_strv_length((gchar **)allowed);
	guint n;
	guint i;
	guint j;

	if (axis_json_type(object) != AXIS_JSON_OBJECT) {
		if (problem != NULL && *problem == NULL)
			*problem = g_strdup_printf("%s is not an object", what);
		return FALSE;
	}
	n = axis_json_length(object);
	if (n > n_allowed) {
		if (problem != NULL && *problem == NULL)
			*problem = g_strdup_printf("%s has a member it may not have, or one twice", what);
		return FALSE;
	}
	for (i = 0; i < n; i++) {
		const gchar *name = axis_json_member_name(object, i);

		if (!g_strv_contains(allowed, name)) {
			if (problem != NULL && *problem == NULL)
				*problem = g_strdup_printf("%s has a member it may not have", what);
			return FALSE;
		}
		for (j = 0; j < i; j++) {
			if (strcmp(name, axis_json_member_name(object, j)) == 0) {
				if (problem != NULL && *problem == NULL)
					*problem = g_strdup_printf("%s has a member twice", what);
				return FALSE;
			}
		}
	}
	return TRUE;
}

static gboolean
get_int(
	const AxisJsonNode  *object,
	const gchar         *name,
	gint64               minimum,
	gint64               maximum,
	gboolean             required,
	gint64              *value,
	gchar              **problem
){
	const AxisJsonNode *node = axis_json_member(object, name);

	if (node == NULL)
		return required ? refuse_member(problem, name, "is missing") : TRUE;
	if (axis_json_type(node) != AXIS_JSON_NUMBER || !axis_json_get_integer(node, minimum, maximum, value))
		return refuse_member(problem, name, "is not a whole number within its range");
	return TRUE;
}

static gboolean
get_uint(
	const AxisJsonNode  *object,
	const gchar         *name,
	guint                minimum,
	guint                maximum,
	gboolean             required,
	guint               *value,
	gchar              **problem
){
	gint64 v = 0;

	if (!get_int(object, name, minimum, maximum, required, &v, problem))
		return FALSE;
	if (axis_json_member(object, name) != NULL)
		*value = (guint)v;
	return TRUE;
}

static gboolean
get_bool(
	const AxisJsonNode  *object,
	const gchar         *name,
	gboolean             required,
	gboolean            *value,
	gchar              **problem
){
	const AxisJsonNode *node = axis_json_member(object, name);

	if (node == NULL)
		return required ? refuse_member(problem, name, "is missing") : TRUE;
	if (axis_json_type(node) != AXIS_JSON_BOOLEAN || !axis_json_get_boolean(node, value))
		return refuse_member(problem, name, "is not true or false");
	return TRUE;
}

static gboolean
get_double(
	const AxisJsonNode  *object,
	const gchar         *name,
	gdouble              minimum,
	gdouble              maximum,
	gboolean             required,
	gboolean            *present,
	gdouble             *value,
	gchar              **problem
){
	const AxisJsonNode *node = axis_json_member(object, name);

	*present = FALSE;
	if (node == NULL)
		return required ? refuse_member(problem, name, "is missing") : TRUE;
	if (axis_json_type(node) != AXIS_JSON_NUMBER || !axis_json_get_double(node, minimum, maximum, value))
		return refuse_member(problem, name, "is not a number within its range");
	*present = TRUE;
	return TRUE;
}

/* A string of at most @max_bytes; with @nullable, null gives NULL. */
static gboolean
get_string(
	const AxisJsonNode  *object,
	const gchar         *name,
	gsize                max_bytes,
	gboolean             required,
	gboolean             nullable,
	const gchar        **value,
	gchar              **problem
){
	const AxisJsonNode *node = axis_json_member(object, name);
	const gchar *text;

	*value = NULL;
	if (node == NULL)
		return required ? refuse_member(problem, name, "is missing") : TRUE;
	if (nullable && axis_json_type(node) == AXIS_JSON_NULL)
		return TRUE;
	text = axis_json_string(node);
	if (text == NULL)
		return refuse_member(problem, name, "is not a string");
	if (strlen(text) > max_bytes)
		return refuse_member(problem, name, "is too long");
	*value = text;
	return TRUE;
}

/* A free text: 1 to @max_chars characters, handed out cleaned. */
static gboolean
get_text(
	const AxisJsonNode  *object,
	const gchar         *name,
	guint                max_chars,
	gboolean             required,
	gboolean             nullable,
	gchar              **value,
	gchar              **problem
){
	const gchar *text = NULL;

	*value = NULL;
	if (!get_string(object, name, (gsize)max_chars * 4, required, nullable, &text, problem))
		return FALSE;
	if (text == NULL)
		return TRUE;
	if (text[0] == '\0' || g_utf8_strlen(text, -1) > (glong)max_chars)
		return refuse_member(problem, name, "is empty or too long");
	*value = axis_library_text_clean(text, max_chars);
	return TRUE;
}

static gboolean
get_array(
	const AxisJsonNode   *object,
	const gchar          *name,
	guint                 min_length,
	guint                 max_length,
	const AxisJsonNode  **value,
	gchar               **problem
){
	const AxisJsonNode *node = axis_json_member(object, name);

	*value = NULL;
	if (node == NULL)
		return refuse_member(problem, name, "is missing");
	if (axis_json_type(node) != AXIS_JSON_ARRAY)
		return refuse_member(problem, name, "is not a list");
	if (axis_json_length(node) < min_length || axis_json_length(node) > max_length)
		return refuse_member(problem, name, "has too few or too many entries");
	*value = node;
	return TRUE;
}

/* "protocol" is a version from @oldest to @newest; *@version (optional) is
 * which. */
static gboolean
check_version(
	const AxisJsonNode  *object,
	guint                oldest,
	guint                newest,
	guint               *version,
	gchar              **problem
){
	gint64 value = 0;

	if (!get_int(object, "protocol", 0, 1000, TRUE, &value, problem))
		return FALSE;
	if (value < (gint64)oldest || value > (gint64)newest)
		return refuse(problem, "the message is of another protocol version");
	if (version != NULL)
		*version = (guint)value;
	return TRUE;
}

static gboolean
check_protocol(
	const AxisJsonNode  *object,
	gchar              **problem
){
	return check_version(object, AXIS_WORKER_PROTOCOL, AXIS_WORKER_PROTOCOL, NULL, problem);
}

/* A list of distinct strings each passing @valid. */
static gboolean
get_names(
	const AxisJsonNode   *object,
	const gchar          *name,
	guint                 min_length,
	guint                 max_length,
	gboolean            (*valid)(const gchar *text),
	gchar              ***value,
	gchar               **problem
){
	const AxisJsonNode *list = NULL;
	GPtrArray *names;
	guint i;
	guint j;

	*value = NULL;
	if (!get_array(object, name, min_length, max_length, &list, problem))
		return FALSE;
	names = g_ptr_array_new_with_free_func(g_free);
	for (i = 0; i < axis_json_length(list); i++) {
		const gchar *text = axis_json_string(axis_json_element(list, i));

		if (text == NULL || !valid(text)) {
			g_ptr_array_unref(names);
			return refuse_member(problem, name, "has an entry that is not acceptable");
		}
		for (j = 0; j < names->len; j++) {
			if (strcmp(text, g_ptr_array_index(names, j)) == 0) {
				g_ptr_array_unref(names);
				return refuse_member(problem, name, "has an entry twice");
			}
		}
		g_ptr_array_add(names, g_strdup(text));
	}
	g_ptr_array_add(names, NULL);
	*value = (gchar **)g_ptr_array_free(names, FALSE);
	return TRUE;
}

static gboolean
get_lease(
	const AxisJsonNode  *object,
	gint64              *job,
	gchar              **lease_token,
	gchar              **problem
){
	const gchar *token = NULL;

	if (!get_int(object, "job", 1, AXIS_WORKER_MAX_ID, TRUE, job, problem) ||
	    !get_string(object, "lease_token", AXIS_VOLUME_UUID_LEN, TRUE, FALSE, &token, problem))
		return FALSE;
	if (!axis_worker_uuid_is_valid(token))
		return refuse_member(problem, "lease_token", "is not a lease token");
	*lease_token = g_strdup(token);
	return TRUE;
}

/* ── Capabilities ───────────────────────────────────────────────── */

static void
write_caps(
	Writer                        *w,
	const AxisEncoderCapabilities *caps
){
	const AxisConvertEncoder *table = axis_convert_encoders();
	gint64 seconds = caps->measured_at_us / G_USEC_PER_SEC;
	guint i;

	w_open(w, '{');
	w_key(w, "measured_at");
	w_int(w, CLAMP(seconds, 0, AXIS_WORKER_MAX_TIME_S));
	w_key(w, "aac");
	w_bool(w, caps->aac);
	w_key(w, "mov_text");
	w_bool(w, caps->mov_text);
	w_key(w, "subrip");
	w_bool(w, caps->subrip);
	w_key(w, "encoders");
	w_open(w, '[');
	for (i = 0; i < AXIS_CONVERT_N_ENCODERS; i++) {
		const AxisEncoderCapability *cap = &caps->encoders[i];
		g_autofree gchar *reason = cap->reason != NULL ?
			axis_library_text_clean(cap->reason, AXIS_WORKER_REASON_MAX_CHARS) : NULL;

		w_open(w, '{');
		w_key(w, "name");
		w_string(w, table[i].name);
		w_key(w, "measured");
		w_bool(w, cap->measured);
		w_key(w, "usable");
		w_bool(w, cap->measured && cap->usable);
		w_key(w, "ten_bit");
		w_bool(w, cap->measured && cap->usable && cap->ten_bit && table[i].ten_bit);
		w_key(w, "hdr10");
		w_bool(w, cap->measured && cap->usable && cap->ten_bit && table[i].ten_bit && cap->hdr10);
		w_key(w, "reason");
		if (reason != NULL && reason[0] != '\0')
			w_string(w, reason);
		else
			w_null(w);
		w_close(w, '}');
	}
	w_close(w, ']');
	w_close(w, '}');
}

static AxisEncoderCapabilities *
read_caps(
	const AxisJsonNode  *node,
	const gchar         *version,
	const gchar         *digest,
	gchar              **problem
){
	static const gchar *const members[] = { "measured_at", "aac", "mov_text", "subrip", "encoders", NULL };
	static const gchar *const encoder_members[] = {
		"name", "measured", "usable", "ten_bit", "hdr10", "reason", NULL
	};
	g_autoptr(AxisEncoderCapabilities) caps = axis_encoder_capabilities_new();
	const AxisJsonNode *list = NULL;
	gboolean seen[AXIS_CONVERT_N_ENCODERS];
	gint64 measured_at = 0;
	guint i;

	if (!only_members(node, "the capabilities", members, problem) ||
	    !get_int(node, "measured_at", 0, AXIS_WORKER_MAX_TIME_S, TRUE, &measured_at, problem) ||
	    !get_bool(node, "aac", TRUE, &caps->aac, problem) ||
	    !get_bool(node, "mov_text", TRUE, &caps->mov_text, problem) ||
	    !get_bool(node, "subrip", TRUE, &caps->subrip, problem) ||
	    !get_array(node, "encoders", 0, AXIS_CONVERT_N_ENCODERS, &list, problem))
		return NULL;
	caps->measured_at_us = measured_at * G_USEC_PER_SEC;
	caps->tool_version = g_strdup(version);
	caps->tool_digest = g_strdup(digest);
	memset(seen, 0, sizeof(seen));
	for (i = 0; i < axis_json_length(list); i++) {
		const AxisJsonNode *entry = axis_json_element(list, i);
		const AxisConvertEncoder *encoder;
		AxisEncoderCapability *cap;
		const gchar *name = NULL;
		gchar *reason = NULL;
		gboolean measured = FALSE;
		gboolean usable = FALSE;
		gboolean ten_bit = FALSE;
		gboolean hdr10 = FALSE;
		guint index;

		if (!only_members(entry, "an encoder's capability", encoder_members, problem) ||
		    !get_string(entry, "name", 32, TRUE, FALSE, &name, problem) ||
		    !get_bool(entry, "measured", TRUE, &measured, problem) ||
		    !get_bool(entry, "usable", TRUE, &usable, problem) ||
		    !get_bool(entry, "ten_bit", TRUE, &ten_bit, problem) ||
		    !get_bool(entry, "hdr10", TRUE, &hdr10, problem) ||
		    !get_text(entry, "reason", AXIS_WORKER_REASON_MAX_CHARS, TRUE, TRUE, &reason, problem))
			return NULL;
		encoder = axis_convert_encoder_named(name);
		if (encoder == NULL) {
			g_free(reason);
			refuse(problem, "the capabilities name an encoder that is not in the table");
			return NULL;
		}
		index = axis_convert_encoder_index(encoder);
		if (seen[index]) {
			g_free(reason);
			refuse(problem, "the capabilities name an encoder twice");
			return NULL;
		}
		seen[index] = TRUE;
		/* Each needs the one before it. */
		if ((usable && !measured) || (ten_bit && (!usable || !encoder->ten_bit)) || (hdr10 && !ten_bit)) {
			g_free(reason);
			refuse(problem, "an encoder's capability claims more than what it needs");
			return NULL;
		}
		cap = &caps->encoders[index];
		cap->measured = measured;
		cap->usable = usable;
		cap->ten_bit = ten_bit;
		cap->hdr10 = hdr10;
		cap->reason = reason;
	}
	return g_steal_pointer(&caps);
}

/* ffmpeg_version, ffmpeg_digest and capabilities, members of @object. */
static AxisEncoderCapabilities *
read_tool(
	const AxisJsonNode  *object,
	gchar              **problem
){
	const gchar *version = NULL;
	const gchar *digest = NULL;
	const AxisJsonNode *caps = axis_json_member(object, "capabilities");

	if (!get_string(object, "ffmpeg_version", AXIS_WORKER_TOOL_VERSION_MAX, TRUE, FALSE, &version, problem) ||
	    !get_string(object, "ffmpeg_digest", 64, TRUE, FALSE, &digest, problem))
		return NULL;
	if (!axis_worker_version_is_valid(version, AXIS_WORKER_TOOL_VERSION_MAX)) {
		refuse_member(problem, "ffmpeg_version", "is not a version");
		return NULL;
	}
	if (!axis_worker_digest_is_valid(digest)) {
		refuse_member(problem, "ffmpeg_digest", "is not a SHA-256 in hexadecimal");
		return NULL;
	}
	if (caps == NULL) {
		refuse_member(problem, "capabilities", "is missing");
		return NULL;
	}
	return read_caps(caps, version, digest, problem);
}

static void
write_tool(
	Writer                        *w,
	const AxisEncoderCapabilities *caps
){
	w_key(w, "ffmpeg_version");
	w_string(w, caps->tool_version);
	w_key(w, "ffmpeg_digest");
	w_string(w, caps->tool_digest);
	w_key(w, "capabilities");
	write_caps(w, caps);
}

gchar *
axis_worker_tool_to_json(const AxisEncoderCapabilities *caps)
{
	Writer w;

	g_return_val_if_fail(caps != NULL, NULL);

	w_begin_version(&w, AXIS_WORKER_TOOL_FORMAT);
	write_tool(&w, caps);
	w_close(&w, '}');
	return w_finish(&w);
}

AxisEncoderCapabilities *
axis_worker_tool_parse(
	const gchar  *data,
	gsize         length,
	gchar       **problem
){
	static const gchar *const members[] = { "protocol", "ffmpeg_version", "ffmpeg_digest", "capabilities", NULL };
	g_autoptr(AxisJsonDoc) doc = read_doc(data, length, AXIS_WORKER_MAX_RESPONSE_BYTES, problem);
	const AxisJsonNode *root;

	if (doc == NULL)
		return NULL;
	root = axis_json_doc_get_root(doc);
	if (!only_members(root, "the measurement", members, problem) ||
	    !check_version(root, AXIS_WORKER_TOOL_FORMAT, AXIS_WORKER_TOOL_FORMAT, NULL, problem))
		return NULL;
	return read_tool(root, problem);
}

/* ── register ───────────────────────────────────────────────────── */

static void
volume_seen_free(gpointer data)
{
	AxisWorkerVolumeSeen *v = data;

	g_free(v->uuid);
	g_free(v->path);
	g_free(v);
}

AxisWorkerRegister *
axis_worker_register_new(void)
{
	AxisWorkerRegister *self = g_new0(AxisWorkerRegister, 1);

	self->volumes = g_ptr_array_new_with_free_func(volume_seen_free);
	return self;
}

void
axis_worker_register_free(AxisWorkerRegister *self)
{
	if (self == NULL)
		return;
	g_free(self->worker_version);
	axis_encoder_capabilities_free(self->caps);
	g_ptr_array_unref(self->volumes);
	g_free(self);
}

gchar *
axis_worker_register_to_json(const AxisWorkerRegister *self)
{
	Writer w;
	guint i;

	g_return_val_if_fail(self != NULL && self->caps != NULL, NULL);

	w_begin_message(&w);
	w_key(&w, "worker_version");
	w_string(&w, self->worker_version);
	w_key(&w, "max_jobs");
	w_uint(&w, self->max_jobs);
	write_tool(&w, self->caps);
	w_key(&w, "volumes");
	w_open(&w, '[');
	for (i = 0; i < self->volumes->len; i++) {
		const AxisWorkerVolumeSeen *v = g_ptr_array_index(self->volumes, i);

		w_open(&w, '{');
		w_key(&w, "uuid");
		w_string(&w, v->uuid);
		w_key(&w, "path");
		w_string(&w, v->path);
		w_key(&w, "keeps_file_identity");
		w_bool(&w, v->keeps_identity);
		w_close(&w, '}');
	}
	w_close(&w, ']');
	w_close(&w, '}');
	return w_finish(&w);
}

AxisWorkerRegister *
axis_worker_register_parse(
	const gchar  *data,
	gsize         length,
	gchar       **problem
){
	static const gchar *const members[] = {
		"protocol", "worker_version", "max_jobs", "ffmpeg_version", "ffmpeg_digest", "capabilities", "volumes",
		NULL
	};
	static const gchar *const volume_members[] = { "uuid", "path", "keeps_file_identity", NULL };
	g_autoptr(AxisJsonDoc) doc = read_doc(data, length, AXIS_WORKER_MAX_REQUEST_BYTES, problem);
	g_autoptr(AxisWorkerRegister) self = NULL;
	const AxisJsonNode *root;
	const AxisJsonNode *volumes = NULL;
	const gchar *version = NULL;
	guint i;
	guint j;

	if (doc == NULL)
		return NULL;
	root = axis_json_doc_get_root(doc);
	self = axis_worker_register_new();
	if (!only_members(root, "the register request", members, problem) || !check_protocol(root, problem) ||
	    !get_string(root, "worker_version", AXIS_WORKER_VERSION_MAX, TRUE, FALSE, &version, problem) ||
	    !get_uint(root, "max_jobs", 1, AXIS_WORKER_MAX_JOBS, TRUE, &self->max_jobs, problem) ||
	    !get_array(root, "volumes", 0, AXIS_WORKER_MAX_VOLUMES, &volumes, problem))
		return NULL;
	if (!axis_worker_version_is_valid(version, AXIS_WORKER_VERSION_MAX)) {
		refuse_member(problem, "worker_version", "is not a version");
		return NULL;
	}
	self->worker_version = g_strdup(version);
	self->caps = read_tool(root, problem);
	if (self->caps == NULL)
		return NULL;
	for (i = 0; i < axis_json_length(volumes); i++) {
		const AxisJsonNode *entry = axis_json_element(volumes, i);
		g_autofree gchar *normalised = NULL;
		AxisWorkerVolumeSeen *seen;
		const gchar *uuid = NULL;
		const gchar *path = NULL;
		gboolean keeps = TRUE;

		if (!only_members(entry, "a volume", volume_members, problem) ||
		    !get_string(entry, "uuid", AXIS_VOLUME_UUID_LEN, TRUE, FALSE, &uuid, problem) ||
		    !get_string(entry, "path", AXIS_STORAGE_PATH_MAX, TRUE, FALSE, &path, problem) ||
		    !get_bool(entry, "keeps_file_identity", TRUE, &keeps, problem))
			return NULL;
		if (!axis_worker_uuid_is_valid(uuid)) {
			refuse(problem, "a volume's uuid is not a volume identifier");
			return NULL;
		}
		if (!axis_storage_path_normalize(path, TRUE, &normalised, NULL) || strcmp(normalised, path) != 0) {
			refuse(problem, "a volume's path is not an absolute path in its normalised form");
			return NULL;
		}
		for (j = 0; j < self->volumes->len; j++) {
			const AxisWorkerVolumeSeen *other = g_ptr_array_index(self->volumes, j);

			if (strcmp(other->uuid, uuid) == 0) {
				refuse(problem, "a volume is reported twice");
				return NULL;
			}
		}
		seen = g_new0(AxisWorkerVolumeSeen, 1);
		seen->uuid = g_strdup(uuid);
		seen->path = g_strdup(path);
		seen->keeps_identity = keeps;
		g_ptr_array_add(self->volumes, seen);
	}
	return g_steal_pointer(&self);
}

static void
volume_answer_free(gpointer data)
{
	AxisWorkerVolumeAnswer *v = data;

	g_free(v->uuid);
	g_free(v->reason);
	g_free(v);
}

AxisWorkerRegistered *
axis_worker_registered_new(void)
{
	AxisWorkerRegistered *self = g_new0(AxisWorkerRegistered, 1);

	self->volumes = g_ptr_array_new_with_free_func(volume_answer_free);
	return self;
}

void
axis_worker_registered_free(AxisWorkerRegistered *self)
{
	if (self == NULL)
		return;
	g_free(self->worker);
	g_ptr_array_unref(self->volumes);
	g_free(self);
}

void
axis_worker_registered_add_volume(
	AxisWorkerRegistered *self,
	const gchar          *uuid,
	gboolean              accepted,
	const gchar          *reason
){
	AxisWorkerVolumeAnswer *answer = g_new0(AxisWorkerVolumeAnswer, 1);

	answer->uuid = g_strdup(uuid);
	answer->accepted = accepted;
	answer->reason = reason != NULL ? axis_library_text_clean(reason, AXIS_WORKER_ANSWER_MAX_CHARS) : NULL;
	g_ptr_array_add(self->volumes, answer);
}

gchar *
axis_worker_registered_to_json(const AxisWorkerRegistered *self)
{
	Writer w;
	guint i;

	w_begin_message(&w);
	w_key(&w, "worker");
	w_string(&w, self->worker);
	w_key(&w, "lease_ms");
	w_uint(&w, self->lease_ms);
	w_key(&w, "heartbeat_ms");
	w_uint(&w, self->heartbeat_ms);
	w_key(&w, "poll_ms");
	w_uint(&w, self->poll_ms);
	w_key(&w, "volumes");
	w_open(&w, '[');
	for (i = 0; i < self->volumes->len; i++) {
		const AxisWorkerVolumeAnswer *v = g_ptr_array_index(self->volumes, i);

		w_open(&w, '{');
		w_key(&w, "uuid");
		w_string(&w, v->uuid);
		w_key(&w, "accepted");
		w_bool(&w, v->accepted);
		w_key(&w, "reason");
		if (v->reason != NULL && v->reason[0] != '\0')
			w_string(&w, v->reason);
		else
			w_null(&w);
		w_close(&w, '}');
	}
	w_close(&w, ']');
	w_close(&w, '}');
	return w_finish(&w);
}

/* The lease and heartbeat of an answer agree: a heartbeat at most a third
 * of the lease, so that two can be missed before it ends. */
static gboolean
check_timing(
	guint    lease_ms,
	guint    heartbeat_ms,
	gchar  **problem
){
	if (heartbeat_ms > lease_ms / 3)
		return refuse(problem, "the heartbeat interval is longer than a third of the lease");
	return TRUE;
}

AxisWorkerRegistered *
axis_worker_registered_parse(
	const gchar  *data,
	gsize         length,
	gchar       **problem
){
	static const gchar *const members[] = {
		"protocol", "worker", "lease_ms", "heartbeat_ms", "poll_ms", "volumes", NULL
	};
	static const gchar *const volume_members[] = { "uuid", "accepted", "reason", NULL };
	g_autoptr(AxisJsonDoc) doc = read_doc(data, length, AXIS_WORKER_MAX_RESPONSE_BYTES, problem);
	g_autoptr(AxisWorkerRegistered) self = NULL;
	const AxisJsonNode *root;
	const AxisJsonNode *volumes = NULL;
	const gchar *name = NULL;
	guint i;
	guint j;

	if (doc == NULL)
		return NULL;
	root = axis_json_doc_get_root(doc);
	self = axis_worker_registered_new();
	if (!only_members(root, "the register answer", members, problem) || !check_protocol(root, problem) ||
	    !get_string(root, "worker", AXIS_WORKER_NAME_MAX, TRUE, FALSE, &name, problem) ||
	    !get_uint(root, "lease_ms", AXIS_WORKER_MIN_LEASE_MS, AXIS_WORKER_MAX_LEASE_MS, TRUE, &self->lease_ms,
	              problem) ||
	    !get_uint(root, "heartbeat_ms", AXIS_WORKER_MIN_HEARTBEAT_MS, AXIS_WORKER_MAX_LEASE_MS, TRUE,
	              &self->heartbeat_ms, problem) ||
	    !get_uint(root, "poll_ms", AXIS_WORKER_MIN_POLL_MS, AXIS_WORKER_MAX_POLL_MS, TRUE, &self->poll_ms, problem) ||
	    !check_timing(self->lease_ms, self->heartbeat_ms, problem) ||
	    !get_array(root, "volumes", 0, AXIS_WORKER_MAX_VOLUMES, &volumes, problem))
		return NULL;
	if (!axis_worker_name_is_valid(name)) {
		refuse_member(problem, "worker", "is not a worker's name");
		return NULL;
	}
	self->worker = g_strdup(name);
	for (i = 0; i < axis_json_length(volumes); i++) {
		const AxisJsonNode *entry = axis_json_element(volumes, i);
		AxisWorkerVolumeAnswer *answer;
		const gchar *uuid = NULL;
		gchar *reason = NULL;
		gboolean accepted = FALSE;

		if (!only_members(entry, "a volume", volume_members, problem) ||
		    !get_string(entry, "uuid", AXIS_VOLUME_UUID_LEN, TRUE, FALSE, &uuid, problem) ||
		    !get_bool(entry, "accepted", TRUE, &accepted, problem) ||
		    !get_text(entry, "reason", AXIS_WORKER_ANSWER_MAX_CHARS, TRUE, TRUE, &reason, problem))
			return NULL;
		if (!axis_worker_uuid_is_valid(uuid)) {
			g_free(reason);
			refuse(problem, "a volume's uuid is not a volume identifier");
			return NULL;
		}
		for (j = 0; j < self->volumes->len; j++) {
			const AxisWorkerVolumeAnswer *other = g_ptr_array_index(self->volumes, j);

			if (strcmp(other->uuid, uuid) == 0) {
				g_free(reason);
				refuse(problem, "a volume is answered twice");
				return NULL;
			}
		}
		answer = g_new0(AxisWorkerVolumeAnswer, 1);
		answer->uuid = g_strdup(uuid);
		answer->accepted = accepted;
		answer->reason = reason;
		g_ptr_array_add(self->volumes, answer);
	}
	return g_steal_pointer(&self);
}

/* ── claim ──────────────────────────────────────────────────────── */

void
axis_worker_claim_free(AxisWorkerClaim *self)
{
	if (self == NULL)
		return;
	g_strfreev(self->kinds);
	g_strfreev(self->volumes);
	g_free(self->paused_reason);
	g_free(self);
}

static void
write_names(
	Writer             *w,
	const gchar *const *names
){
	guint i;

	w_open(w, '[');
	for (i = 0; names != NULL && names[i] != NULL; i++)
		w_string(w, names[i]);
	w_close(w, ']');
}

gchar *
axis_worker_claim_to_json(const AxisWorkerClaim *self)
{
	Writer w;

	w_begin_message(&w);
	w_key(&w, "kinds");
	write_names(&w, (const gchar *const *)self->kinds);
	w_key(&w, "volumes");
	write_names(&w, (const gchar *const *)self->volumes);
	if (self->paused_job > 0) {
		g_autofree gchar *clean = axis_library_text_clean(self->paused_reason != NULL &&
			self->paused_reason[0] != '\0' ? self->paused_reason : "its encoder crashed", AXIS_WORKER_PAUSE_MAX_CHARS);

		w_key(&w, "paused");
		w_open(&w, '{');
		w_key(&w, "job");
		w_int(&w, self->paused_job);
		w_key(&w, "reason");
		w_string(&w, clean != NULL && clean[0] != '\0' ? clean : "its encoder crashed");
		w_close(&w, '}');
	}
	w_close(&w, '}');
	return w_finish(&w);
}

AxisWorkerClaim *
axis_worker_claim_parse(
	const gchar  *data,
	gsize         length,
	gchar       **problem
){
	static const gchar *const members[] = { "protocol", "kinds", "volumes", "paused", NULL };
	static const gchar *const paused_members[] = { "job", "reason", NULL };
	g_autoptr(AxisJsonDoc) doc = read_doc(data, length, AXIS_WORKER_MAX_REQUEST_BYTES, problem);
	g_autoptr(AxisWorkerClaim) self = NULL;
	const AxisJsonNode *root;
	const AxisJsonNode *paused;

	if (doc == NULL)
		return NULL;
	root = axis_json_doc_get_root(doc);
	self = g_new0(AxisWorkerClaim, 1);
	if (!only_members(root, "the claim request", members, problem) || !check_protocol(root, problem) ||
	    !get_names(root, "kinds", 1, AXIS_WORKER_MAX_KINDS, axis_worker_kind_is_valid, &self->kinds, problem) ||
	    !get_names(root, "volumes", 0, AXIS_WORKER_MAX_VOLUMES, axis_worker_uuid_is_valid, &self->volumes,
	               problem))
		return NULL;
	/* Optional: present only while the worker is paused. */
	paused = axis_json_member(root, "paused");
	if (paused != NULL) {
		if (!only_members(paused, "the pause", paused_members, problem) ||
		    !get_int(paused, "job", 1, AXIS_WORKER_MAX_ID, TRUE, &self->paused_job, problem) ||
		    !get_text(paused, "reason", AXIS_WORKER_PAUSE_MAX_CHARS, TRUE, FALSE, &self->paused_reason, problem))
			return NULL;
	}
	return g_steal_pointer(&self);
}

/* ── The sample description ─────────────────────────────────────── */

static void
file_plan_free(gpointer data)
{
	AxisSampleFilePlan *f = data;

	g_free(f->volume);
	g_free(f->root);
	g_free(f->path);
	g_free(f);
}

AxisSampleDescription *
axis_sample_description_new(void)
{
	AxisSampleDescription *self = g_new0(AxisSampleDescription, 1);

	self->files = g_ptr_array_new_with_free_func(file_plan_free);
	self->audio_quality = -1;
	return self;
}

void
axis_sample_description_free(AxisSampleDescription *self)
{
	if (self == NULL)
		return;
	g_ptr_array_unref(self->files);
	g_free(self);
}

void
axis_sample_description_add_file(
	AxisSampleDescription    *self,
	const AxisSampleFilePlan *file
){
	AxisSampleFilePlan *copy = g_new0(AxisSampleFilePlan, 1);

	*copy = *file;
	copy->index = self->files->len;
	copy->volume = g_strdup(file->volume);
	copy->root = g_strdup(file->root);
	copy->path = g_strdup(file->path);
	g_ptr_array_add(self->files, copy);
}

static gboolean
codec_of_media(
	AxisTargetMedia media,
	AxisTargetCodec codec
){
	gboolean video = codec == AXIS_TCODEC_HEVC || codec == AXIS_TCODEC_H264;

	return media == AXIS_TARGET_VIDEO ? video : !video;
}

/* One file's checks, shared by the parser and axis_sample_description_check(). */
static gboolean
check_file(
	const AxisSampleDescription  *self,
	const AxisSampleFilePlan     *f,
	guint                         position,
	gchar                       **problem
){
	const gchar *why = NULL;
	guint k;

	if (f->index != position)
		return refuse(problem, "a file's index is not its place in the list");
	if (!axis_worker_uuid_is_valid(f->volume))
		return refuse(problem, "a file's volume is not a volume identifier");
	if (!axis_storage_file_path_valid(f->root, f->path, &why))
		return refuse(problem, why != NULL ? why : "a file's path is not acceptable");
	if (f->size < 0 || f->size > AXIS_SAMPLE_MAX_FILE_BYTES || f->mtime_nsec < 0 || f->mtime_nsec > 999999999 ||
	    f->mtime_sec < -AXIS_WORKER_MAX_TIME_S || f->mtime_sec > AXIS_WORKER_MAX_TIME_S)
		return refuse(problem, "a file's size or time is out of range");
	if (f->stream < 0 || f->stream > AXIS_SAMPLE_MAX_STREAM || f->quality < AXIS_QUALITY_MIN ||
	    f->quality > AXIS_QUALITY_MAX)
		return refuse(problem, "a file's stream or quality is out of range");
	if (self->media == AXIS_TARGET_VIDEO) {
		if (f->bit_depth != 8 && !(f->bit_depth == 10 && self->codec == AXIS_TCODEC_HEVC))
			return refuse(problem, "a picture's bit depth is not one the codec is encoded to");
	} else if (f->bit_depth != 0) {
		return refuse(problem, "an audio file has a picture's bit depth");
	}
	if (f->channels < 0 || f->channels > AXIS_SAMPLE_MAX_CHANNELS || f->sample_rate < 0 ||
	    f->sample_rate > AXIS_SAMPLE_MAX_SAMPLE_RATE || f->sample_bits < 0 || f->sample_bits > AXIS_SAMPLE_MAX_SAMPLE_BITS)
		return refuse(problem, "a file's audio facts are out of range");
	if (f->n_excerpts < 1 || f->n_excerpts > AXIS_SAMPLE_MAX_EXCERPTS || f->length_ms < AXIS_SAMPLE_MIN_EXCERPT_MS ||
	    f->length_ms > AXIS_SAMPLE_MAX_EXCERPT_MS)
		return refuse(problem, "a file's excerpts are out of range");
	for (k = 0; k < f->n_excerpts; k++) {
		if (f->start_ms[k] < 0 || f->start_ms[k] > AXIS_SAMPLE_MAX_START_MS)
			return refuse(problem, "an excerpt's start is out of range");
	}
	return TRUE;
}

gboolean
axis_sample_description_check(
	const AxisSampleDescription  *self,
	gchar                       **problem
){
	guint i;

	if ((guint)self->media > AXIS_TARGET_AUDIO || (guint)self->codec >= AXIS_TCODEC_COUNT ||
	    (guint)self->family >= AXIS_ENCODER_COUNT || !codec_of_media(self->media, self->codec))
		return refuse(problem, "the target's media, codec or family is not acceptable");
	if (self->target_quality < AXIS_QUALITY_MIN || self->target_quality > AXIS_QUALITY_MAX ||
	    self->audio_bitrate < 0 || self->audio_bitrate > AXIS_SAMPLE_MAX_AUDIO_BITRATE ||
	    self->audio_bitrate_mono < 0 || self->audio_bitrate_mono > AXIS_SAMPLE_MAX_AUDIO_BITRATE ||
	    self->audio_quality < -1 || self->audio_quality > 9)
		return refuse(problem, "the target's numbers are out of range");
	if (self->excerpt_timeout_ms < AXIS_SAMPLE_MIN_TIMEOUT_MS || self->excerpt_timeout_ms > AXIS_SAMPLE_MAX_TIMEOUT_MS)
		return refuse(problem, "the excerpt time limit is out of range");
	if (self->files == NULL || self->files->len < 1 || self->files->len > AXIS_SAMPLE_MAX_FILES)
		return refuse(problem, "the measurement has no files, or too many");
	for (i = 0; i < self->files->len; i++) {
		if (!check_file(self, g_ptr_array_index(self->files, i), i, problem))
			return FALSE;
	}
	return TRUE;
}

void
axis_sample_description_to_target(
	const AxisSampleDescription *self,
	const AxisSampleFilePlan    *file,
	AxisEstimateTarget          *target,
	AxisMediaStream             *stream
){
	memset(target, 0, sizeof(*target));
	memset(stream, 0, sizeof(*stream));
	target->media = self->media;
	target->codec = self->codec;
	target->encoder = self->family;
	target->quality = self->target_quality;
	target->audio_bitrate = self->audio_bitrate;
	target->audio_bitrate_mono = self->audio_bitrate_mono;
	target->audio_quality = self->audio_quality;
	target->reduce_to_cd = self->reduce_to_cd;
	stream->index = file->stream;
	stream->channels = file->channels;
	stream->sample_rate = file->sample_rate;
	stream->bit_depth_audio = file->sample_bits;
}

static void
write_description(
	Writer                      *w,
	const AxisSampleDescription *self
){
	guint i;
	guint k;

	w_open(w, '{');
	w_key(w, "media");
	w_string(w, self->media == AXIS_TARGET_VIDEO ? "video" : "audio");
	w_key(w, "codec");
	w_string(w, axis_target_codec_to_string(self->codec));
	w_key(w, "family");
	w_string(w, axis_encoder_family_to_string(self->family));
	w_key(w, "target_quality");
	w_int(w, self->target_quality);
	w_key(w, "audio_bitrate");
	w_int(w, self->audio_bitrate);
	w_key(w, "audio_bitrate_mono");
	w_int(w, self->audio_bitrate_mono);
	w_key(w, "audio_quality");
	w_int(w, self->audio_quality);
	w_key(w, "reduce_to_cd");
	w_bool(w, self->reduce_to_cd);
	w_key(w, "excerpt_timeout_ms");
	w_uint(w, self->excerpt_timeout_ms);
	w_key(w, "files");
	w_open(w, '[');
	for (i = 0; i < self->files->len; i++) {
		const AxisSampleFilePlan *f = g_ptr_array_index(self->files, i);

		w_open(w, '{');
		w_key(w, "index");
		w_uint(w, f->index);
		w_key(w, "volume");
		w_string(w, f->volume);
		w_key(w, "root");
		w_string(w, f->root);
		w_key(w, "path");
		w_string(w, f->path);
		w_key(w, "size");
		w_int(w, f->size);
		w_key(w, "mtime_sec");
		w_int(w, f->mtime_sec);
		w_key(w, "mtime_nsec");
		w_int(w, f->mtime_nsec);
		w_key(w, "stream");
		w_int(w, f->stream);
		w_key(w, "quality");
		w_int(w, f->quality);
		w_key(w, "bit_depth");
		w_int(w, f->bit_depth);
		w_key(w, "channels");
		w_int(w, f->channels);
		w_key(w, "sample_rate");
		w_int(w, f->sample_rate);
		w_key(w, "sample_bits");
		w_int(w, f->sample_bits);
		w_key(w, "length_ms");
		w_int(w, f->length_ms);
		w_key(w, "starts_ms");
		w_open(w, '[');
		for (k = 0; k < f->n_excerpts; k++)
			w_int(w, f->start_ms[k]);
		w_close(w, ']');
		w_close(w, '}');
	}
	w_close(w, ']');
	w_close(w, '}');
}

static gboolean
read_int_field(
	const AxisJsonNode  *object,
	const gchar         *name,
	gint64               minimum,
	gint64               maximum,
	gint                *value,
	gchar              **problem
){
	gint64 v = 0;

	if (!get_int(object, name, minimum, maximum, TRUE, &v, problem))
		return FALSE;
	*value = (gint)v;
	return TRUE;
}

static AxisSampleDescription *
read_description(
	const AxisJsonNode  *node,
	gchar              **problem
){
	static const gchar *const members[] = {
		"media", "codec", "family", "target_quality", "audio_bitrate", "audio_bitrate_mono", "audio_quality",
		"reduce_to_cd", "excerpt_timeout_ms", "files", NULL
	};
	static const gchar *const file_members[] = {
		"index", "volume", "root", "path", "size", "mtime_sec", "mtime_nsec", "stream", "quality", "bit_depth",
		"channels", "sample_rate", "sample_bits", "length_ms", "starts_ms", NULL
	};
	g_autoptr(AxisSampleDescription) self = axis_sample_description_new();
	const AxisJsonNode *files = NULL;
	const gchar *media = NULL;
	const gchar *codec = NULL;
	const gchar *family = NULL;
	guint i;
	guint k;

	if (!only_members(node, "the description", members, problem) ||
	    !get_string(node, "media", 8, TRUE, FALSE, &media, problem) ||
	    !get_string(node, "codec", 8, TRUE, FALSE, &codec, problem) ||
	    !get_string(node, "family", 16, TRUE, FALSE, &family, problem) ||
	    !read_int_field(node, "target_quality", AXIS_QUALITY_MIN, AXIS_QUALITY_MAX, &self->target_quality, problem) ||
	    !read_int_field(node, "audio_bitrate", 0, AXIS_SAMPLE_MAX_AUDIO_BITRATE, &self->audio_bitrate, problem) ||
	    !read_int_field(node, "audio_bitrate_mono", 0, AXIS_SAMPLE_MAX_AUDIO_BITRATE, &self->audio_bitrate_mono,
	                    problem) ||
	    !read_int_field(node, "audio_quality", -1, 9, &self->audio_quality, problem) ||
	    !get_bool(node, "reduce_to_cd", TRUE, &self->reduce_to_cd, problem) ||
	    !get_uint(node, "excerpt_timeout_ms", AXIS_SAMPLE_MIN_TIMEOUT_MS, AXIS_SAMPLE_MAX_TIMEOUT_MS, TRUE,
	              &self->excerpt_timeout_ms, problem) ||
	    !get_array(node, "files", 1, AXIS_SAMPLE_MAX_FILES, &files, problem))
		return NULL;
	if (strcmp(media, "video") == 0)
		self->media = AXIS_TARGET_VIDEO;
	else if (strcmp(media, "audio") == 0)
		self->media = AXIS_TARGET_AUDIO;
	else {
		refuse_member(problem, "media", "is neither video nor audio");
		return NULL;
	}
	if (!axis_target_codec_from_string(codec, &self->codec) || !codec_of_media(self->media, self->codec)) {
		refuse_member(problem, "codec", "is not a codec of the media");
		return NULL;
	}
	if (!axis_encoder_family_from_string(family, &self->family)) {
		refuse_member(problem, "family", "is not an encoder family");
		return NULL;
	}
	for (i = 0; i < axis_json_length(files); i++) {
		const AxisJsonNode *entry = axis_json_element(files, i);
		const AxisJsonNode *starts = NULL;
		AxisSampleFilePlan f;
		const gchar *volume = NULL;
		const gchar *root = NULL;
		const gchar *path = NULL;
		guint index = 0;

		memset(&f, 0, sizeof(f));
		if (!only_members(entry, "a file", file_members, problem) ||
		    !get_uint(entry, "index", 0, AXIS_SAMPLE_MAX_FILES, TRUE, &index, problem) ||
		    !get_string(entry, "volume", AXIS_VOLUME_UUID_LEN, TRUE, FALSE, &volume, problem) ||
		    !get_string(entry, "root", AXIS_STORAGE_PATH_MAX, TRUE, FALSE, &root, problem) ||
		    !get_string(entry, "path", 4 * AXIS_LIBRARY_PATH_MAX, TRUE, FALSE, &path, problem) ||
		    !get_int(entry, "size", 0, AXIS_SAMPLE_MAX_FILE_BYTES, TRUE, &f.size, problem) ||
		    !get_int(entry, "mtime_sec", -AXIS_WORKER_MAX_TIME_S, AXIS_WORKER_MAX_TIME_S, TRUE, &f.mtime_sec,
		             problem) ||
		    !read_int_field(entry, "mtime_nsec", 0, 999999999, &f.mtime_nsec, problem) ||
		    !read_int_field(entry, "stream", 0, AXIS_SAMPLE_MAX_STREAM, &f.stream, problem) ||
		    !read_int_field(entry, "quality", AXIS_QUALITY_MIN, AXIS_QUALITY_MAX, &f.quality, problem) ||
		    !read_int_field(entry, "bit_depth", 0, 10, &f.bit_depth, problem) ||
		    !read_int_field(entry, "channels", 0, AXIS_SAMPLE_MAX_CHANNELS, &f.channels, problem) ||
		    !read_int_field(entry, "sample_rate", 0, AXIS_SAMPLE_MAX_SAMPLE_RATE, &f.sample_rate, problem) ||
		    !read_int_field(entry, "sample_bits", 0, AXIS_SAMPLE_MAX_SAMPLE_BITS, &f.sample_bits, problem) ||
		    !get_int(entry, "length_ms", AXIS_SAMPLE_MIN_EXCERPT_MS, AXIS_SAMPLE_MAX_EXCERPT_MS, TRUE, &f.length_ms,
		             problem) ||
		    !get_array(entry, "starts_ms", 1, AXIS_SAMPLE_MAX_EXCERPTS, &starts, problem))
			return NULL;
		f.index = index;
		f.n_excerpts = axis_json_length(starts);
		for (k = 0; k < f.n_excerpts; k++) {
			const AxisJsonNode *start = axis_json_element(starts, k);

			if (axis_json_type(start) != AXIS_JSON_NUMBER ||
			    !axis_json_get_integer(start, 0, AXIS_SAMPLE_MAX_START_MS, &f.start_ms[k])) {
				refuse(problem, "an excerpt's start is not a whole number within its range");
				return NULL;
			}
		}
		f.volume = (gchar *)volume;
		f.root = (gchar *)root;
		f.path = (gchar *)path;
		if (!check_file(self, &f, i, problem))
			return NULL;
		axis_sample_description_add_file(self, &f);
	}
	return g_steal_pointer(&self);
}

gchar *
axis_sample_description_to_json(const AxisSampleDescription *self)
{
	Writer w;

	w_init(&w);
	write_description(&w, self);
	return w_finish(&w);
}

AxisSampleDescription *
axis_sample_description_parse(
	const gchar  *data,
	gsize         length,
	gchar       **problem
){
	g_autoptr(AxisJsonDoc) doc = read_doc(data, length, AXIS_WORKER_MAX_RESPONSE_BYTES, problem);

	if (doc == NULL)
		return NULL;
	return read_description(axis_json_doc_get_root(doc), problem);
}

/* ── A conversion ───────────────────────────────────────────────── */

/* 1 to @max digits, no leading zero; *@end after them. */
static gboolean
digits(
	const gchar  *p,
	guint         max,
	const gchar **end
){
	guint n = 0;

	if (*p == '0')
		return FALSE;
	while (g_ascii_isdigit(p[n]) && n <= max)
		n++;
	if (n == 0 || n > max)
		return FALSE;
	*end = p + n;
	return TRUE;
}

gboolean
axis_convert_output_name_valid(const gchar *name)
{
	const gchar *p = NULL;

	if (name == NULL || !digits(name, 19, &p) || *p != '-' || !digits(p + 1, 3, &p))
		return FALSE;
	return strcmp(p, ".tmp") == 0;
}

void
axis_convert_work_free(AxisConvertWork *self)
{
	if (self == NULL)
		return;
	g_free(self->volume);
	g_free(self->root);
	g_free(self->path);
	g_free(self->source_sample);
	g_free(self->output);
	g_strfreev(self->args);
	g_free(self);
}

/* An argument: 1 to AXIS_CONVERT_MAX_ARG_BYTES of valid UTF-8 with no
 * control character. */
static gboolean
convert_arg_valid(const gchar *arg)
{
	const gchar *p;
	gsize n;

	if (arg == NULL)
		return FALSE;
	n = strlen(arg);
	if (n == 0 || n > AXIS_CONVERT_MAX_ARG_BYTES || !g_utf8_validate(arg, (gssize)n, NULL))
		return FALSE;
	for (p = arg; *p != '\0'; p = g_utf8_next_char(p)) {
		gunichar c = g_utf8_get_char(p);

		if (c < 0x20 || (c >= 0x7f && c < 0xa0))
			return FALSE;
	}
	return TRUE;
}

gboolean
axis_convert_work_check(
	const AxisConvertWork  *self,
	gchar                 **problem
){
	const gchar *why = NULL;
	guint n;

	if (self == NULL)
		return refuse(problem, "there is no description");
	if (!axis_worker_uuid_is_valid(self->volume))
		return refuse(problem, "the volume is not a volume identifier");
	if (!axis_storage_file_path_valid(self->root, self->path, &why))
		return refuse(problem, why != NULL ? why : "the file's path is not acceptable");
	if (self->size < 1 || self->size > AXIS_SAMPLE_MAX_FILE_BYTES || self->mtime_nsec < 0 ||
	    self->mtime_nsec > 999999999 || self->mtime_sec < -AXIS_WORKER_MAX_TIME_S ||
	    self->mtime_sec > AXIS_WORKER_MAX_TIME_S || self->inode < 0)
		return refuse(problem, "the file's size, time or inode is out of range");
	if (!axis_worker_digest_is_valid(self->source_sample))
		return refuse(problem, "the digest of the source's samples is not a SHA-256 in hexadecimal");
	if (!axis_convert_output_name_valid(self->output))
		return refuse(problem, "the output's name is not one of the work area's");
	if (self->args == NULL)
		return refuse(problem, "there are no arguments");
	for (n = 0; self->args[n] != NULL; n++) {
		if (n >= AXIS_CONVERT_MAX_ARGS || !convert_arg_valid(self->args[n]))
			return refuse(problem, "an argument is not acceptable, or there are too many");
	}
	if (n == 0)
		return refuse(problem, "there are no arguments");
	if (self->source_streams < 1 || self->source_streams > AXIS_PROBE_MAX_STREAMS)
		return refuse(problem, "the number of the source's streams is out of range");
	if (self->duration_ms < 0 || self->duration_ms > AXIS_CONVERT_MAX_DURATION_MS)
		return refuse(problem, "the duration is out of range");
	if (self->timeout_ms < AXIS_CONVERT_MIN_TIMEOUT_MS || self->timeout_ms > AXIS_CONVERT_MAX_TIMEOUT_MS)
		return refuse(problem, "the time limit is out of range");
	return TRUE;
}

static void
write_convert(
	Writer                *w,
	const AxisConvertWork *self
){
	guint i;

	w_open(w, '{');
	w_key(w, "volume");
	w_string(w, self->volume);
	w_key(w, "root");
	w_string(w, self->root);
	w_key(w, "path");
	w_string(w, self->path);
	w_key(w, "size");
	w_int(w, self->size);
	w_key(w, "mtime_sec");
	w_int(w, self->mtime_sec);
	w_key(w, "mtime_nsec");
	w_int(w, self->mtime_nsec);
	w_key(w, "inode");
	w_int(w, self->inode);
	w_key(w, "source_sample");
	w_string(w, self->source_sample);
	w_key(w, "output");
	w_string(w, self->output);
	w_key(w, "args");
	w_open(w, '[');
	for (i = 0; self->args != NULL && self->args[i] != NULL; i++)
		w_string(w, self->args[i]);
	w_close(w, ']');
	w_key(w, "source_streams");
	w_uint(w, self->source_streams);
	w_key(w, "duration_ms");
	w_int(w, self->duration_ms);
	w_key(w, "timeout_ms");
	w_uint(w, self->timeout_ms);
	w_close(w, '}');
}

static AxisConvertWork *
read_convert(
	const AxisJsonNode  *node,
	gchar              **problem
){
	static const gchar *const members[] = {
		"volume", "root", "path", "size", "mtime_sec", "mtime_nsec", "inode", "source_sample", "output", "args",
		"source_streams", "duration_ms", "timeout_ms", NULL
	};
	g_autoptr(AxisConvertWork) self = g_new0(AxisConvertWork, 1);
	const AxisJsonNode *args = NULL;
	const gchar *volume = NULL;
	const gchar *root = NULL;
	const gchar *path = NULL;
	const gchar *sample = NULL;
	const gchar *output = NULL;
	GPtrArray *list;
	guint i;

	if (!only_members(node, "the description", members, problem) ||
	    !get_string(node, "volume", AXIS_VOLUME_UUID_LEN, TRUE, FALSE, &volume, problem) ||
	    !get_string(node, "root", AXIS_STORAGE_PATH_MAX, TRUE, FALSE, &root, problem) ||
	    !get_string(node, "path", 4 * AXIS_LIBRARY_PATH_MAX, TRUE, FALSE, &path, problem) ||
	    !get_int(node, "size", 1, AXIS_SAMPLE_MAX_FILE_BYTES, TRUE, &self->size, problem) ||
	    !get_int(node, "mtime_sec", -AXIS_WORKER_MAX_TIME_S, AXIS_WORKER_MAX_TIME_S, TRUE, &self->mtime_sec, problem) ||
	    !read_int_field(node, "mtime_nsec", 0, 999999999, &self->mtime_nsec, problem) ||
	    !get_int(node, "inode", 0, AXIS_CONVERT_MAX_INODE, TRUE, &self->inode, problem) ||
	    !get_string(node, "source_sample", 64, TRUE, FALSE, &sample, problem) ||
	    !get_string(node, "output", 32, TRUE, FALSE, &output, problem) ||
	    !get_array(node, "args", 1, AXIS_CONVERT_MAX_ARGS, &args, problem) ||
	    !get_uint(node, "source_streams", 1, AXIS_PROBE_MAX_STREAMS, TRUE, &self->source_streams, problem) ||
	    !get_int(node, "duration_ms", 0, AXIS_CONVERT_MAX_DURATION_MS, TRUE, &self->duration_ms, problem) ||
	    !get_uint(node, "timeout_ms", AXIS_CONVERT_MIN_TIMEOUT_MS, AXIS_CONVERT_MAX_TIMEOUT_MS, TRUE, &self->timeout_ms,
	              problem))
		return NULL;
	list = g_ptr_array_new_with_free_func(g_free);
	for (i = 0; i < axis_json_length(args); i++) {
		const gchar *arg = axis_json_string(axis_json_element(args, i));

		if (!convert_arg_valid(arg)) {
			g_ptr_array_unref(list);
			refuse_member(problem, "args", "has an argument that is not acceptable");
			return NULL;
		}
		g_ptr_array_add(list, g_strdup(arg));
	}
	g_ptr_array_add(list, NULL);
	self->args = (gchar **)g_ptr_array_free(list, FALSE);
	self->volume = g_strdup(volume);
	self->root = g_strdup(root);
	self->path = g_strdup(path);
	self->source_sample = g_strdup(sample);
	self->output = g_strdup(output);
	if (!axis_convert_work_check(self, problem))
		return NULL;
	return g_steal_pointer(&self);
}

void
axis_convert_result_free(AxisConvertResult *self)
{
	if (self == NULL)
		return;
	g_free(self->output);
	g_free(self);
}

static AxisConvertResult *
read_convert_result(
	const AxisJsonNode  *node,
	gchar              **problem
){
	static const gchar *const members[] = { "output", "size", NULL };
	g_autoptr(AxisConvertResult) self = g_new0(AxisConvertResult, 1);
	const gchar *output = NULL;

	if (!only_members(node, "the result", members, problem) ||
	    !get_string(node, "output", 32, TRUE, FALSE, &output, problem) ||
	    !get_int(node, "size", 1, AXIS_SAMPLE_MAX_FILE_BYTES, TRUE, &self->size, problem))
		return NULL;
	if (!axis_convert_output_name_valid(output)) {
		refuse_member(problem, "output", "is not a name of the work area's");
		return NULL;
	}
	self->output = g_strdup(output);
	return g_steal_pointer(&self);
}

/* ── A converted file's checks ──────────────────────────────────── */

static void
write_plan(
	Writer               *w,
	const AxisVerifyPlan *plan
){
	guint k;

	w_open(w, '{');
	w_key(w, "picture");
	w_open(w, '{');
	w_key(w, "source");
	w_int(w, plan->picture_source);
	w_key(w, "output");
	w_uint(w, plan->picture_output);
	w_close(w, '}');
	w_key(w, "tracks");
	w_open(w, '[');
	for (k = 0; k < plan->n_tracks; k++) {
		w_open(w, '{');
		w_key(w, "source");
		w_int(w, plan->tracks[k].source);
		w_key(w, "output");
		w_uint(w, plan->tracks[k].output);
		w_key(w, "aac");
		w_bool(w, plan->tracks[k].aac);
		w_close(w, '}');
	}
	w_close(w, ']');
	w_key(w, "points");
	w_open(w, '[');
	for (k = 0; k < plan->n_points; k++) {
		w_open(w, '{');
		w_key(w, "start_ms");
		w_int(w, plan->points[k].start_ms);
		w_key(w, "length_ms");
		w_int(w, plan->points[k].length_ms);
		w_close(w, '}');
	}
	w_close(w, ']');
	w_key(w, "timeout_ms");
	w_uint(w, plan->timeout_ms);
	w_close(w, '}');
}

static AxisVerifyPlan *
read_plan(
	const AxisJsonNode  *node,
	gchar              **problem
){
	static const gchar *const members[] = { "picture", "tracks", "points", "timeout_ms", NULL };
	static const gchar *const picture_members[] = { "source", "output", NULL };
	static const gchar *const track_members[] = { "source", "output", "aac", NULL };
	static const gchar *const point_members[] = { "start_ms", "length_ms", NULL };
	g_autoptr(AxisVerifyPlan) plan = axis_verify_plan_new();
	const AxisJsonNode *picture;
	const AxisJsonNode *tracks = NULL;
	const AxisJsonNode *points = NULL;
	gint64 value = 0;
	guint k;

	if (!only_members(node, "the plan of the checks", members, problem) ||
	    !get_array(node, "tracks", 0, AXIS_VERIFY_MAX_TRACKS, &tracks, problem) ||
	    !get_array(node, "points", 0, AXIS_VERIFY_MAX_POINTS, &points, problem) ||
	    !get_uint(node, "timeout_ms", AXIS_VERIFY_MIN_TIMEOUT_MS, AXIS_VERIFY_MAX_TIMEOUT_MS, TRUE, &plan->timeout_ms,
	              problem))
		return NULL;
	picture = axis_json_member(node, "picture");
	if (picture == NULL) {
		refuse_member(problem, "picture", "is missing");
		return NULL;
	}
	if (!only_members(picture, "the picture", picture_members, problem) ||
	    !read_int_field(picture, "source", 0, AXIS_VERIFY_MAX_TRACKS - 1, &plan->picture_source, problem) ||
	    !get_int(picture, "output", 0, AXIS_VERIFY_MAX_TRACKS - 1, TRUE, &value, problem))
		return NULL;
	plan->picture_output = (guint)value;
	for (k = 0; k < axis_json_length(tracks); k++) {
		const AxisJsonNode *entry = axis_json_element(tracks, k);
		gint source = 0;
		gint64 output = 0;
		gboolean aac = FALSE;

		if (!only_members(entry, "a copied track", track_members, problem) ||
		    !read_int_field(entry, "source", 0, AXIS_VERIFY_MAX_TRACKS - 1, &source, problem) ||
		    !get_int(entry, "output", 0, AXIS_VERIFY_MAX_TRACKS - 1, TRUE, &output, problem) ||
		    !get_bool(entry, "aac", TRUE, &aac, problem))
			return NULL;
		axis_verify_plan_add_track(plan, source, (guint)output, aac);
	}
	for (k = 0; k < axis_json_length(points); k++) {
		const AxisJsonNode *entry = axis_json_element(points, k);

		if (!only_members(entry, "a point", point_members, problem) ||
		    !get_int(entry, "start_ms", 0, AXIS_VERIFY_MAX_START_MS, TRUE, &plan->points[k].start_ms, problem) ||
		    !get_int(entry, "length_ms", 1, 60000, TRUE, &plan->points[k].length_ms, problem))
			return NULL;
	}
	plan->n_points = axis_json_length(points);
	if (!axis_verify_plan_check(plan, problem))
		return NULL;
	return g_steal_pointer(&plan);
}

gchar *
axis_verify_plan_to_json(const AxisVerifyPlan *plan)
{
	Writer w;

	g_return_val_if_fail(plan != NULL, NULL);

	w_init(&w);
	write_plan(&w, plan);
	return w_finish(&w);
}

AxisVerifyPlan *
axis_verify_plan_parse(
	const gchar  *data,
	gsize         length,
	gchar       **problem
){
	g_autoptr(AxisJsonDoc) doc = read_doc(data, length, AXIS_WORKER_MAX_RESPONSE_BYTES, problem);

	if (doc == NULL)
		return NULL;
	return read_plan(axis_json_doc_get_root(doc), problem);
}

void
axis_verify_work_free(AxisVerifyWork *self)
{
	if (self == NULL)
		return;
	g_free(self->volume);
	g_free(self->root);
	g_free(self->path);
	g_free(self->source_sample);
	g_free(self->output);
	axis_verify_plan_free(self->plan);
	g_free(self);
}

gboolean
axis_verify_work_check(
	const AxisVerifyWork  *self,
	gchar                **problem
){
	const gchar *why = NULL;

	if (self == NULL)
		return refuse(problem, "there is no description");
	if (!axis_worker_uuid_is_valid(self->volume))
		return refuse(problem, "the volume is not a volume identifier");
	if (!axis_storage_file_path_valid(self->root, self->path, &why))
		return refuse(problem, why != NULL ? why : "the file's path is not acceptable");
	if (self->size < 1 || self->size > AXIS_SAMPLE_MAX_FILE_BYTES || self->mtime_nsec < 0 ||
	    self->mtime_nsec > 999999999 || self->mtime_sec < -AXIS_WORKER_MAX_TIME_S ||
	    self->mtime_sec > AXIS_WORKER_MAX_TIME_S || self->inode < 0)
		return refuse(problem, "the file's size, time or inode is out of range");
	if (!axis_worker_digest_is_valid(self->source_sample))
		return refuse(problem, "the digest of the source's samples is not a SHA-256 in hexadecimal");
	if (!axis_convert_output_name_valid(self->output))
		return refuse(problem, "the output's name is not one of the work area's");
	if (self->output_size < 1 || self->output_size > AXIS_SAMPLE_MAX_FILE_BYTES)
		return refuse(problem, "the output's size is out of range");
	return axis_verify_plan_check(self->plan, problem);
}

static void
write_verify(
	Writer               *w,
	const AxisVerifyWork *self
){
	w_open(w, '{');
	w_key(w, "volume");
	w_string(w, self->volume);
	w_key(w, "root");
	w_string(w, self->root);
	w_key(w, "path");
	w_string(w, self->path);
	w_key(w, "size");
	w_int(w, self->size);
	w_key(w, "mtime_sec");
	w_int(w, self->mtime_sec);
	w_key(w, "mtime_nsec");
	w_int(w, self->mtime_nsec);
	w_key(w, "inode");
	w_int(w, self->inode);
	w_key(w, "source_sample");
	w_string(w, self->source_sample);
	w_key(w, "output");
	w_string(w, self->output);
	w_key(w, "output_size");
	w_int(w, self->output_size);
	w_key(w, "plan");
	write_plan(w, self->plan);
	w_close(w, '}');
}

static AxisVerifyWork *
read_verify(
	const AxisJsonNode  *node,
	gchar              **problem
){
	static const gchar *const members[] = {
		"volume", "root", "path", "size", "mtime_sec", "mtime_nsec", "inode", "source_sample", "output", "output_size",
		"plan", NULL
	};
	g_autoptr(AxisVerifyWork) self = g_new0(AxisVerifyWork, 1);
	const AxisJsonNode *plan;
	const gchar *volume = NULL;
	const gchar *root = NULL;
	const gchar *path = NULL;
	const gchar *sample = NULL;
	const gchar *output = NULL;

	if (!only_members(node, "the description", members, problem) ||
	    !get_string(node, "volume", AXIS_VOLUME_UUID_LEN, TRUE, FALSE, &volume, problem) ||
	    !get_string(node, "root", AXIS_STORAGE_PATH_MAX, TRUE, FALSE, &root, problem) ||
	    !get_string(node, "path", 4 * AXIS_LIBRARY_PATH_MAX, TRUE, FALSE, &path, problem) ||
	    !get_int(node, "size", 1, AXIS_SAMPLE_MAX_FILE_BYTES, TRUE, &self->size, problem) ||
	    !get_int(node, "mtime_sec", -AXIS_WORKER_MAX_TIME_S, AXIS_WORKER_MAX_TIME_S, TRUE, &self->mtime_sec, problem) ||
	    !read_int_field(node, "mtime_nsec", 0, 999999999, &self->mtime_nsec, problem) ||
	    !get_int(node, "inode", 0, AXIS_CONVERT_MAX_INODE, TRUE, &self->inode, problem) ||
	    !get_string(node, "source_sample", 64, TRUE, FALSE, &sample, problem) ||
	    !get_string(node, "output", 32, TRUE, FALSE, &output, problem) ||
	    !get_int(node, "output_size", 1, AXIS_SAMPLE_MAX_FILE_BYTES, TRUE, &self->output_size, problem))
		return NULL;
	plan = axis_json_member(node, "plan");
	if (plan == NULL) {
		refuse_member(problem, "plan", "is missing");
		return NULL;
	}
	self->plan = read_plan(plan, problem);
	if (self->plan == NULL)
		return NULL;
	self->volume = g_strdup(volume);
	self->root = g_strdup(root);
	self->path = g_strdup(path);
	self->source_sample = g_strdup(sample);
	self->output = g_strdup(output);
	if (!axis_verify_work_check(self, problem))
		return NULL;
	return g_steal_pointer(&self);
}

void
axis_verify_result_free(AxisVerifyResult *self)
{
	if (self == NULL)
		return;
	g_free(self->output);
	axis_verify_evidence_free(self->evidence);
	g_free(self);
}

static void
write_run(
	Writer              *w,
	const AxisVerifyRun *run
){
	w_open(w, '{');
	w_key(w, "status");
	w_string(w, axis_verify_status_to_string(run->status));
	w_key(w, "code");
	w_int(w, run->code);
	w_key(w, "error_bytes");
	w_uint(w, run->error_bytes);
	w_key(w, "said");
	if (run->said != NULL && run->said[0] != '\0') {
		g_autofree gchar *clean = axis_library_text_clean(run->said, AXIS_VERIFY_SAID_MAX_CHARS);

		if (clean != NULL && clean[0] != '\0')
			w_string(w, clean);
		else
			w_null(w);
	} else {
		w_null(w);
	}
	w_close(w, '}');
}

static void
write_hashes(
	Writer *w,
	gchar **hashes
){
	guint i;

	if (hashes == NULL) {
		w_null(w);
		return;
	}
	w_open(w, '[');
	for (i = 0; hashes[i] != NULL; i++)
		w_string(w, hashes[i]);
	w_close(w, ']');
}

static void
write_count(
	Writer                *w,
	const AxisVerifyCount *count
){
	w_open(w, '{');
	w_key(w, "run");
	write_run(w, &count->run);
	w_key(w, "count");
	if (count->read)
		w_uint(w, count->count);
	else
		w_null(w);
	w_key(w, "bytes");
	if (count->read)
		w_uint(w, count->bytes);
	else
		w_null(w);
	w_close(w, '}');
}

static gboolean
result_writable(const AxisVerifyResult *self)
{
	const AxisVerifyEvidence *e = self->evidence;
	guint k;

	if (e == NULL || !e->decoded || !e->counted || !e->hashed || e->n_points > AXIS_VERIFY_MAX_POINTS ||
	    !axis_worker_digest_is_valid(e->output_digest) || !axis_worker_digest_is_valid(e->output_sample) ||
	    !axis_worker_digest_is_valid(e->source_sample) || !axis_convert_output_name_valid(self->output))
		return FALSE;
	if (!axis_verify_run_reportable(&e->decode) || !axis_verify_run_reportable(&e->tracks) ||
	    !axis_verify_run_reportable(&e->source_packets.run) || !axis_verify_run_reportable(&e->output_packets.run))
		return FALSE;
	for (k = 0; k < e->n_points; k++) {
		if (!axis_verify_run_reportable(&e->points[k].run))
			return FALSE;
	}
	return TRUE;
}

static void
write_result(
	Writer                 *w,
	const AxisVerifyResult *self
){
	const AxisVerifyEvidence *e = self->evidence;
	guint k;

	w_open(w, '{');
	w_key(w, "output");
	w_string(w, self->output);
	w_key(w, "output_size");
	w_int(w, self->output_size);
	w_key(w, "output_digest");
	w_string(w, e->output_digest);
	w_key(w, "output_sample");
	w_string(w, e->output_sample);
	w_key(w, "source_sample");
	w_string(w, e->source_sample);
	w_key(w, "decode");
	w_open(w, '{');
	w_key(w, "run");
	write_run(w, &e->decode);
	w_key(w, "hashes");
	write_hashes(w, e->output_hashes);
	w_close(w, '}');
	w_key(w, "packets");
	w_open(w, '{');
	w_key(w, "source");
	write_count(w, &e->source_packets);
	w_key(w, "output");
	write_count(w, &e->output_packets);
	w_close(w, '}');
	w_key(w, "tracks");
	w_open(w, '{');
	w_key(w, "run");
	write_run(w, &e->tracks);
	w_key(w, "hashes");
	write_hashes(w, e->source_hashes);
	w_close(w, '}');
	w_key(w, "points");
	w_open(w, '[');
	for (k = 0; k < e->n_points; k++) {
		const AxisVerifyPicture *p = &e->points[k];

		w_open(w, '{');
		w_key(w, "run");
		write_run(w, &p->run);
		w_key(w, "lowest");
		if (p->read)
			w_double(w, p->lowest);
		else
			w_null(w);
		w_key(w, "frames");
		w_uint(w, p->frames);
		w_close(w, '}');
	}
	w_close(w, ']');
	w_close(w, '}');
}

gchar *
axis_verify_result_to_json(const AxisVerifyResult *self)
{
	Writer w;

	g_return_val_if_fail(self != NULL, NULL);

	if (!result_writable(self))
		return NULL;
	w_init(&w);
	write_result(&w, self);
	return w_finish(&w);
}

static gboolean
read_run(
	const AxisJsonNode  *object,
	const gchar         *name,
	AxisVerifyRun       *run,
	gchar              **problem
){
	static const gchar *const members[] = { "status", "code", "error_bytes", "said", NULL };
	const AxisJsonNode *node = axis_json_member(object, name);
	const gchar *status = NULL;
	gint64 code = 0;
	gint64 bytes = 0;

	axis_verify_run_clear(run);
	if (node == NULL)
		return refuse_member(problem, name, "is missing");
	if (!only_members(node, "a tool's run", members, problem) ||
	    !get_string(node, "status", 16, TRUE, FALSE, &status, problem) ||
	    !get_int(node, "code", 0, 255, TRUE, &code, problem) ||
	    !get_int(node, "error_bytes", 0, 1024 * 1024, TRUE, &bytes, problem) ||
	    !get_text(node, "said", AXIS_VERIFY_SAID_MAX_CHARS, TRUE, TRUE, &run->said, problem))
		return FALSE;
	if (!axis_verify_status_from_string(status, &run->status))
		return refuse(problem, "a tool's run ended in a way this protocol does not have");
	run->code = (gint)code;
	run->error_bytes = (guint64)bytes;
	/* What a measurement may be: not a cancellation, a tool that would not
	 * start or die, or a crash (a worker fails its job for those). */
	if (!axis_verify_run_reportable(run))
		return refuse(problem, "a tool's run ended in a way that is not a measurement");
	/* Words without bytes are not what a tool said (bytes without words
	 * are: control characters only, cleaned away). */
	if (run->said != NULL && run->error_bytes == 0)
		return refuse(problem, "what a tool said does not agree with how much it said");
	return TRUE;
}

/* A list of SHA-256 in lower-case hexadecimal, or null. */
static gboolean
read_hashes(
	const AxisJsonNode   *object,
	const gchar          *name,
	gchar              ***hashes,
	gchar               **problem
){
	const AxisJsonNode *node = axis_json_member(object, name);
	GPtrArray *list;
	guint i;

	*hashes = NULL;
	if (node == NULL)
		return refuse_member(problem, name, "is missing");
	if (axis_json_type(node) == AXIS_JSON_NULL)
		return TRUE;
	if (axis_json_type(node) != AXIS_JSON_ARRAY || axis_json_length(node) > AXIS_VERIFY_MAX_TRACKS)
		return refuse_member(problem, name, "is not a list of hashes within its length");
	list = g_ptr_array_new_with_free_func(g_free);
	for (i = 0; i < axis_json_length(node); i++) {
		const gchar *text = axis_json_string(axis_json_element(node, i));

		if (!axis_worker_digest_is_valid(text)) {
			g_ptr_array_unref(list);
			return refuse_member(problem, name, "has an entry that is not a SHA-256 in hexadecimal");
		}
		g_ptr_array_add(list, g_strdup(text));
	}
	g_ptr_array_add(list, NULL);
	*hashes = (gchar **)g_ptr_array_free(list, FALSE);
	return TRUE;
}

/* A whole number, or null (*@present FALSE). */
static gboolean
get_count(
	const AxisJsonNode  *object,
	const gchar         *name,
	gboolean            *present,
	guint64             *value,
	gchar              **problem
){
	const AxisJsonNode *node = axis_json_member(object, name);
	gint64 v = 0;

	*present = FALSE;
	*value = 0;
	if (node == NULL)
		return refuse_member(problem, name, "is missing");
	if (axis_json_type(node) == AXIS_JSON_NULL)
		return TRUE;
	if (!get_int(object, name, 0, AXIS_VERIFY_MAX_COUNT, TRUE, &v, problem))
		return FALSE;
	*present = TRUE;
	*value = (guint64)v;
	return TRUE;
}

static gboolean
read_count(
	const AxisJsonNode  *object,
	const gchar         *name,
	AxisVerifyCount     *count,
	gchar              **problem
){
	static const gchar *const members[] = { "run", "count", "bytes", NULL };
	const AxisJsonNode *node = axis_json_member(object, name);
	gboolean has_count = FALSE;
	gboolean has_bytes = FALSE;

	if (node == NULL)
		return refuse_member(problem, name, "is missing");
	if (!only_members(node, "a packet count", members, problem) || !read_run(node, "run", &count->run, problem) ||
	    !get_count(node, "count", &has_count, &count->count, problem) ||
	    !get_count(node, "bytes", &has_bytes, &count->bytes, problem))
		return FALSE;
	if (has_count != has_bytes)
		return refuse(problem, "a packet count has its count or its bytes without the other");
	count->read = has_count;
	return TRUE;
}

static AxisVerifyResult *
read_result(
	const AxisJsonNode  *node,
	gchar              **problem
){
	static const gchar *const members[] = {
		"output", "output_size", "output_digest", "output_sample", "source_sample", "decode", "packets", "tracks",
		"points", NULL
	};
	static const gchar *const run_hash_members[] = { "run", "hashes", NULL };
	static const gchar *const packets_members[] = { "source", "output", NULL };
	static const gchar *const point_members[] = { "run", "lowest", "frames", NULL };
	g_autoptr(AxisVerifyResult) self = g_new0(AxisVerifyResult, 1);
	AxisVerifyEvidence *e;
	const AxisJsonNode *part;
	const AxisJsonNode *points = NULL;
	const gchar *output = NULL;
	const gchar *digest = NULL;
	const gchar *output_sample = NULL;
	const gchar *source_sample = NULL;
	guint k;

	self->evidence = axis_verify_evidence_new();
	e = self->evidence;
	if (!only_members(node, "the measurements", members, problem) ||
	    !get_string(node, "output", 32, TRUE, FALSE, &output, problem) ||
	    !get_int(node, "output_size", 1, AXIS_SAMPLE_MAX_FILE_BYTES, TRUE, &self->output_size, problem) ||
	    !get_string(node, "output_digest", 64, TRUE, FALSE, &digest, problem) ||
	    !get_string(node, "output_sample", 64, TRUE, FALSE, &output_sample, problem) ||
	    !get_string(node, "source_sample", 64, TRUE, FALSE, &source_sample, problem) ||
	    !get_array(node, "points", 0, AXIS_VERIFY_MAX_POINTS, &points, problem))
		return NULL;
	if (!axis_convert_output_name_valid(output)) {
		refuse_member(problem, "output", "is not a name of the work area's");
		return NULL;
	}
	if (!axis_worker_digest_is_valid(digest) || !axis_worker_digest_is_valid(output_sample) ||
	    !axis_worker_digest_is_valid(source_sample)) {
		refuse(problem, "a digest is not a SHA-256 in hexadecimal");
		return NULL;
	}
	part = axis_json_member(node, "decode");
	if (part == NULL || !only_members(part, "the decode", run_hash_members, problem) ||
	    !read_run(part, "run", &e->decode, problem) || !read_hashes(part, "hashes", &e->output_hashes, problem)) {
		refuse_member(problem, "decode", "is missing");
		return NULL;
	}
	part = axis_json_member(node, "packets");
	if (part == NULL || !only_members(part, "the packet counts", packets_members, problem) ||
	    !read_count(part, "source", &e->source_packets, problem) ||
	    !read_count(part, "output", &e->output_packets, problem)) {
		refuse_member(problem, "packets", "is missing");
		return NULL;
	}
	part = axis_json_member(node, "tracks");
	if (part == NULL || !only_members(part, "the original's tracks", run_hash_members, problem) ||
	    !read_run(part, "run", &e->tracks, problem) || !read_hashes(part, "hashes", &e->source_hashes, problem)) {
		refuse_member(problem, "tracks", "is missing");
		return NULL;
	}
	for (k = 0; k < axis_json_length(points); k++) {
		const AxisJsonNode *entry = axis_json_element(points, k);
		AxisVerifyPicture *p = &e->points[k];
		const AxisJsonNode *lowest;
		gboolean present = FALSE;
		guint frames = 0;

		if (!only_members(entry, "a point", point_members, problem) || !read_run(entry, "run", &p->run, problem) ||
		    !get_uint(entry, "frames", 0, 10000000, TRUE, &frames, problem))
			return NULL;
		lowest = axis_json_member(entry, "lowest");
		if (lowest == NULL) {
			refuse_member(problem, "lowest", "is missing");
			return NULL;
		}
		if (axis_json_type(lowest) != AXIS_JSON_NULL &&
		    !get_double(entry, "lowest", -1.0, 1.0, TRUE, &present, &p->lowest, problem))
			return NULL;
		p->read = present;
		p->frames = frames;
		if (p->read && frames == 0) {
			refuse(problem, "a point's similarity is given with no frames counted");
			return NULL;
		}
	}
	e->n_points = axis_json_length(points);
	e->decoded = TRUE;
	e->counted = TRUE;
	e->hashed = TRUE;
	e->output_digest = g_strdup(digest);
	e->output_sample = g_strdup(output_sample);
	e->source_sample = g_strdup(source_sample);
	self->output = g_strdup(output);
	return g_steal_pointer(&self);
}

AxisVerifyResult *
axis_verify_result_parse(
	const gchar  *data,
	gsize         length,
	gchar       **problem
){
	g_autoptr(AxisJsonDoc) doc = read_doc(data, length, AXIS_WORKER_MAX_REQUEST_BYTES, problem);

	if (doc == NULL)
		return NULL;
	return read_result(axis_json_doc_get_root(doc), problem);
}

/* ── claimed ────────────────────────────────────────────────────── */

void
axis_worker_job_free(AxisWorkerJob *self)
{
	if (self == NULL)
		return;
	g_free(self->kind);
	g_free(self->lease_token);
	axis_sample_description_free(self->sample);
	axis_convert_work_free(self->convert);
	axis_verify_work_free(self->verify);
	g_free(self);
}

gchar *
axis_worker_claimed_to_json(
	const AxisWorkerJob *job,
	const gchar         *paused
){
	g_autofree gchar *why = paused != NULL ? axis_library_text_clean(paused[0] != '\0' ? paused : "paused",
		AXIS_WORKER_PAUSED_MAX_CHARS) : NULL;
	Writer w;

	/* A worker held paused is given nothing. */
	if (why != NULL)
		job = NULL;
	w_begin_message(&w);
	w_key(&w, "job");
	if (job == NULL) {
		w_null(&w);
	} else {
		w_open(&w, '{');
		w_key(&w, "id");
		w_int(&w, job->id);
		w_key(&w, "kind");
		w_string(&w, job->kind);
		w_key(&w, "lease_token");
		w_string(&w, job->lease_token);
		w_key(&w, "lease_ms");
		w_uint(&w, job->lease_ms);
		w_key(&w, "heartbeat_ms");
		w_uint(&w, job->heartbeat_ms);
		w_key(&w, "attempt");
		w_uint(&w, job->attempt);
		w_key(&w, "description");
		if (job->sample != NULL) {
			write_description(&w, job->sample);
		} else if (job->convert != NULL) {
			write_convert(&w, job->convert);
		} else if (job->verify != NULL) {
			write_verify(&w, job->verify);
		} else {
			w_open(&w, '{');
			w_close(&w, '}');
		}
		w_close(&w, '}');
	}
	w_key(&w, "paused");
	if (why != NULL && why[0] != '\0')
		w_string(&w, why);
	else if (why != NULL)
		w_string(&w, "paused");
	else
		w_null(&w);
	w_close(&w, '}');
	return w_finish(&w);
}

gboolean
axis_worker_claimed_parse(
	const gchar    *data,
	gsize           length,
	AxisWorkerJob **job_out,
	gchar         **paused_out,
	gchar         **problem
){
	static const gchar *const members[] = { "protocol", "job", "paused", NULL };
	static const gchar *const job_members[] = {
		"id", "kind", "lease_token", "lease_ms", "heartbeat_ms", "attempt", "description", NULL
	};
	g_autoptr(AxisJsonDoc) doc = read_doc(data, length, AXIS_WORKER_MAX_RESPONSE_BYTES, problem);
	g_autoptr(AxisWorkerJob) job = NULL;
	g_autofree gchar *paused = NULL;
	const AxisJsonNode *root;
	const AxisJsonNode *node;
	const AxisJsonNode *description;
	const gchar *kind = NULL;
	const gchar *token = NULL;

	*job_out = NULL;
	if (paused_out != NULL)
		*paused_out = NULL;
	if (doc == NULL)
		return FALSE;
	root = axis_json_doc_get_root(doc);
	if (!only_members(root, "the claim answer", members, problem) || !check_protocol(root, problem) ||
	    !get_text(root, "paused", AXIS_WORKER_PAUSED_MAX_CHARS, TRUE, TRUE, &paused, problem))
		return FALSE;
	node = axis_json_member(root, "job");
	if (node == NULL)
		return refuse_member(problem, "job", "is missing");
	if (axis_json_type(node) == AXIS_JSON_NULL) {
		if (paused_out != NULL)
			*paused_out = g_steal_pointer(&paused);
		return TRUE;
	}
	/* A worker held paused is given nothing: an answer that says both is
	 * not one a daemon gives. */
	if (paused != NULL)
		return refuse(problem, "the answer gives work to a worker it says is paused");

	job = g_new0(AxisWorkerJob, 1);
	if (!only_members(node, "the job", job_members, problem) ||
	    !get_int(node, "id", 1, AXIS_WORKER_MAX_ID, TRUE, &job->id, problem) ||
	    !get_string(node, "kind", AXIS_WORKER_KIND_MAX, TRUE, FALSE, &kind, problem) ||
	    !get_string(node, "lease_token", AXIS_VOLUME_UUID_LEN, TRUE, FALSE, &token, problem) ||
	    !get_uint(node, "lease_ms", AXIS_WORKER_MIN_LEASE_MS, AXIS_WORKER_MAX_LEASE_MS, TRUE, &job->lease_ms,
	              problem) ||
	    !get_uint(node, "heartbeat_ms", AXIS_WORKER_MIN_HEARTBEAT_MS, AXIS_WORKER_MAX_LEASE_MS, TRUE,
	              &job->heartbeat_ms, problem) ||
	    !check_timing(job->lease_ms, job->heartbeat_ms, problem) ||
	    !get_uint(node, "attempt", 1, AXIS_WORKER_MAX_ATTEMPT, TRUE, &job->attempt, problem))
		return FALSE;
	if (!axis_worker_uuid_is_valid(token))
		return refuse_member(problem, "lease_token", "is not a lease token");
	job->lease_token = g_strdup(token);
	description = axis_json_member(node, "description");
	if (description == NULL)
		return refuse_member(problem, "description", "is missing");
	if (g_strcmp0(kind, AXIS_WORKER_KIND_SAMPLE) == 0) {
		job->sample = read_description(description, problem);
		if (job->sample == NULL)
			return FALSE;
	} else if (g_strcmp0(kind, AXIS_WORKER_KIND_CONVERT) == 0) {
		job->convert = read_convert(description, problem);
		if (job->convert == NULL)
			return FALSE;
	} else if (g_strcmp0(kind, AXIS_WORKER_KIND_VERIFY) == 0) {
		job->verify = read_verify(description, problem);
		if (job->verify == NULL)
			return FALSE;
	} else if (g_strcmp0(kind, AXIS_WORKER_KIND_CAPABILITIES) == 0) {
		static const gchar *const none[] = { NULL };

		if (!only_members(description, "the description", none, problem))
			return FALSE;
	} else {
		return refuse_member(problem, "kind", "is not a kind of work this protocol has");
	}
	job->kind = g_strdup(kind);
	*job_out = g_steal_pointer(&job);
	return TRUE;
}

/* ── heartbeat ──────────────────────────────────────────────────── */

void
axis_worker_heartbeat_free(AxisWorkerHeartbeat *self)
{
	if (self == NULL)
		return;
	g_free(self->lease_token);
	g_free(self->message);
	g_free(self);
}

gchar *
axis_worker_heartbeat_to_json(const AxisWorkerHeartbeat *self)
{
	Writer w;

	w_begin_message(&w);
	w_key(&w, "job");
	w_int(&w, self->job);
	w_key(&w, "lease_token");
	w_string(&w, self->lease_token);
	if (self->has_progress) {
		w_key(&w, "progress");
		w_double(&w, CLAMP(self->progress, 0.0, 1.0));
	}
	if (self->message != NULL && self->message[0] != '\0') {
		g_autofree gchar *clean = axis_library_text_clean(self->message, AXIS_WORKER_MESSAGE_MAX_CHARS);

		w_key(&w, "message");
		w_string(&w, clean);
	}
	w_close(&w, '}');
	return w_finish(&w);
}

AxisWorkerHeartbeat *
axis_worker_heartbeat_parse(
	const gchar  *data,
	gsize         length,
	gchar       **problem
){
	static const gchar *const members[] = { "protocol", "job", "lease_token", "progress", "message", NULL };
	g_autoptr(AxisJsonDoc) doc = read_doc(data, length, AXIS_WORKER_MAX_REQUEST_BYTES, problem);
	g_autoptr(AxisWorkerHeartbeat) self = NULL;
	const AxisJsonNode *root;

	if (doc == NULL)
		return NULL;
	root = axis_json_doc_get_root(doc);
	self = g_new0(AxisWorkerHeartbeat, 1);
	if (!only_members(root, "the heartbeat", members, problem) || !check_protocol(root, problem) ||
	    !get_lease(root, &self->job, &self->lease_token, problem) ||
	    !get_double(root, "progress", 0.0, 1.0, FALSE, &self->has_progress, &self->progress, problem) ||
	    !get_text(root, "message", AXIS_WORKER_MESSAGE_MAX_CHARS, FALSE, FALSE, &self->message, problem))
		return NULL;
	return g_steal_pointer(&self);
}

void
axis_worker_beat_free(AxisWorkerBeat *self)
{
	if (self == NULL)
		return;
	g_free(self->reason);
	g_free(self);
}

gchar *
axis_worker_beat_to_json(const AxisWorkerBeat *self)
{
	Writer w;

	w_begin_message(&w);
	w_key(&w, "action");
	if (self->stop) {
		w_string(&w, "stop");
		w_key(&w, "reason");
		w_string(&w, g_strcmp0(self->reason, "cancelled") == 0 ? "cancelled" : "lease");
	} else {
		w_string(&w, "continue");
		w_key(&w, "lease_ms");
		w_uint(&w, self->lease_ms);
	}
	w_close(&w, '}');
	return w_finish(&w);
}

AxisWorkerBeat *
axis_worker_beat_parse(
	const gchar  *data,
	gsize         length,
	gchar       **problem
){
	static const gchar *const members[] = { "protocol", "action", "lease_ms", "reason", NULL };
	g_autoptr(AxisJsonDoc) doc = read_doc(data, length, AXIS_WORKER_MAX_RESPONSE_BYTES, problem);
	g_autoptr(AxisWorkerBeat) self = NULL;
	const AxisJsonNode *root;
	const gchar *action = NULL;
	const gchar *reason = NULL;

	if (doc == NULL)
		return NULL;
	root = axis_json_doc_get_root(doc);
	self = g_new0(AxisWorkerBeat, 1);
	if (!only_members(root, "the heartbeat answer", members, problem) || !check_protocol(root, problem) ||
	    !get_string(root, "action", 16, TRUE, FALSE, &action, problem))
		return NULL;
	if (strcmp(action, "continue") == 0) {
		if (axis_json_member(root, "reason") != NULL) {
			refuse_member(problem, "reason", "may only come with stop");
			return NULL;
		}
		if (!get_uint(root, "lease_ms", AXIS_WORKER_MIN_LEASE_MS, AXIS_WORKER_MAX_LEASE_MS, TRUE, &self->lease_ms,
		              problem))
			return NULL;
	} else if (strcmp(action, "stop") == 0) {
		self->stop = TRUE;
		if (axis_json_member(root, "lease_ms") != NULL) {
			refuse_member(problem, "lease_ms", "may only come with continue");
			return NULL;
		}
		if (!get_string(root, "reason", 16, TRUE, FALSE, &reason, problem))
			return NULL;
		if (strcmp(reason, "cancelled") != 0 && strcmp(reason, "lease") != 0) {
			refuse_member(problem, "reason", "is neither cancelled nor lease");
			return NULL;
		}
		self->reason = g_strdup(reason);
	} else {
		refuse_member(problem, "action", "is neither continue nor stop");
		return NULL;
	}
	return g_steal_pointer(&self);
}

/* ── complete ───────────────────────────────────────────────────── */

static void
file_result_free(gpointer data)
{
	AxisSampleFileResult *f = data;

	g_free(f->reason);
	g_free(f);
}

AxisSampleResult *
axis_sample_result_new(void)
{
	AxisSampleResult *self = g_new0(AxisSampleResult, 1);

	self->files = g_ptr_array_new_with_free_func(file_result_free);
	return self;
}

void
axis_sample_result_free(AxisSampleResult *self)
{
	if (self == NULL)
		return;
	g_ptr_array_unref(self->files);
	g_free(self);
}

void
axis_sample_result_add(
	AxisSampleResult *self,
	guint             index,
	const guint64    *bytes,
	guint             n_bytes,
	const gchar      *reason
){
	AxisSampleFileResult *f = g_new0(AxisSampleFileResult, 1);
	guint k;

	f->index = index;
	if (reason != NULL) {
		f->reason = g_strdup(reason);
	} else {
		f->measured = TRUE;
		f->n_bytes = MIN(n_bytes, (guint)AXIS_SAMPLE_MAX_EXCERPTS);
		for (k = 0; k < f->n_bytes; k++)
			f->bytes[k] = bytes[k];
	}
	g_ptr_array_add(self->files, f);
}

void
axis_worker_complete_free(AxisWorkerComplete *self)
{
	if (self == NULL)
		return;
	g_free(self->lease_token);
	g_free(self->kind);
	axis_sample_result_free(self->sample);
	axis_encoder_capabilities_free(self->caps);
	axis_convert_result_free(self->convert);
	axis_verify_result_free(self->verify);
	g_free(self);
}

gchar *
axis_worker_complete_to_json(const AxisWorkerComplete *self)
{
	Writer w;
	guint i;
	guint k;

	/* Only measurements a worker may report are written. */
	if (self->verify != NULL && !result_writable(self->verify))
		return NULL;
	w_begin_message(&w);
	w_key(&w, "job");
	w_int(&w, self->job);
	w_key(&w, "lease_token");
	w_string(&w, self->lease_token);
	w_key(&w, "kind");
	w_string(&w, self->kind);
	w_key(&w, "result");
	if (self->verify != NULL) {
		write_result(&w, self->verify);
		w_close(&w, '}');
		return w_finish(&w);
	}
	w_open(&w, '{');
	if (self->caps != NULL) {
		write_tool(&w, self->caps);
	} else if (self->convert != NULL) {
		w_key(&w, "output");
		w_string(&w, self->convert->output);
		w_key(&w, "size");
		w_int(&w, self->convert->size);
	} else {
		w_key(&w, "files");
		w_open(&w, '[');
		for (i = 0; self->sample != NULL && i < self->sample->files->len; i++) {
			const AxisSampleFileResult *f = g_ptr_array_index(self->sample->files, i);

			w_open(&w, '{');
			w_key(&w, "index");
			w_uint(&w, f->index);
			if (f->measured) {
				w_key(&w, "bytes");
				w_open(&w, '[');
				for (k = 0; k < f->n_bytes; k++)
					w_uint(&w, f->bytes[k]);
				w_close(&w, ']');
			} else {
				w_key(&w, "skipped");
				w_string(&w, f->reason);
			}
			w_close(&w, '}');
		}
		w_close(&w, ']');
	}
	w_close(&w, '}');
	w_close(&w, '}');
	return w_finish(&w);
}

static AxisSampleResult *
read_sample_result(
	const AxisJsonNode  *node,
	gchar              **problem
){
	static const gchar *const members[] = { "files", NULL };
	static const gchar *const measured_members[] = { "index", "bytes", NULL };
	static const gchar *const skipped_members[] = { "index", "skipped", NULL };
	g_autoptr(AxisSampleResult) self = axis_sample_result_new();
	const AxisJsonNode *files = NULL;
	gboolean seen[AXIS_SAMPLE_MAX_FILES];
	guint i;
	guint k;

	if (!only_members(node, "the result", members, problem) ||
	    !get_array(node, "files", 0, AXIS_SAMPLE_MAX_FILES, &files, problem))
		return NULL;
	memset(seen, 0, sizeof(seen));
	for (i = 0; i < axis_json_length(files); i++) {
		const AxisJsonNode *entry = axis_json_element(files, i);
		guint index = 0;

		if (axis_json_member(entry, "bytes") != NULL) {
			const AxisJsonNode *bytes = NULL;
			guint64 counts[AXIS_SAMPLE_MAX_EXCERPTS];

			if (!only_members(entry, "a file's result", measured_members, problem) ||
			    !get_uint(entry, "index", 0, AXIS_SAMPLE_MAX_FILES - 1, TRUE, &index, problem) ||
			    !get_array(entry, "bytes", 1, AXIS_SAMPLE_MAX_EXCERPTS, &bytes, problem))
				return NULL;
			for (k = 0; k < axis_json_length(bytes); k++) {
				const AxisJsonNode *count = axis_json_element(bytes, k);
				gint64 value = 0;

				if (axis_json_type(count) != AXIS_JSON_NUMBER ||
				    !axis_json_get_integer(count, 0, (gint64)AXIS_SAMPLE_OUTPUT_CAP, &value)) {
					refuse(problem, "an excerpt's byte count is not a whole number within its range");
					return NULL;
				}
				counts[k] = (guint64)value;
			}
			if (seen[index]) {
				refuse(problem, "a file's result is given twice");
				return NULL;
			}
			seen[index] = TRUE;
			axis_sample_result_add(self, index, counts, axis_json_length(bytes), NULL);
		} else {
			const gchar *reason = NULL;

			if (!only_members(entry, "a file's result", skipped_members, problem) ||
			    !get_uint(entry, "index", 0, AXIS_SAMPLE_MAX_FILES - 1, TRUE, &index, problem) ||
			    !get_string(entry, "skipped", 32, TRUE, FALSE, &reason, problem))
				return NULL;
			if (!axis_sample_skip_reason_is_valid(reason)) {
				refuse_member(problem, "skipped", "is not a reason this protocol has");
				return NULL;
			}
			if (seen[index]) {
				refuse(problem, "a file's result is given twice");
				return NULL;
			}
			seen[index] = TRUE;
			axis_sample_result_add(self, index, NULL, 0, reason);
		}
	}
	return g_steal_pointer(&self);
}

AxisWorkerComplete *
axis_worker_complete_parse(
	const gchar  *data,
	gsize         length,
	gchar       **problem
){
	static const gchar *const members[] = { "protocol", "job", "lease_token", "kind", "result", NULL };
	static const gchar *const tool_members[] = { "ffmpeg_version", "ffmpeg_digest", "capabilities", NULL };
	g_autoptr(AxisJsonDoc) doc = read_doc(data, length, AXIS_WORKER_MAX_REQUEST_BYTES, problem);
	g_autoptr(AxisWorkerComplete) self = NULL;
	const AxisJsonNode *root;
	const AxisJsonNode *result;
	const gchar *kind = NULL;

	if (doc == NULL)
		return NULL;
	root = axis_json_doc_get_root(doc);
	self = g_new0(AxisWorkerComplete, 1);
	if (!only_members(root, "the complete request", members, problem) || !check_protocol(root, problem) ||
	    !get_lease(root, &self->job, &self->lease_token, problem) ||
	    !get_string(root, "kind", AXIS_WORKER_KIND_MAX, TRUE, FALSE, &kind, problem))
		return NULL;
	result = axis_json_member(root, "result");
	if (result == NULL) {
		refuse_member(problem, "result", "is missing");
		return NULL;
	}
	if (strcmp(kind, AXIS_WORKER_KIND_SAMPLE) == 0) {
		self->sample = read_sample_result(result, problem);
		if (self->sample == NULL)
			return NULL;
	} else if (strcmp(kind, AXIS_WORKER_KIND_CAPABILITIES) == 0) {
		if (!only_members(result, "the result", tool_members, problem))
			return NULL;
		self->caps = read_tool(result, problem);
		if (self->caps == NULL)
			return NULL;
	} else if (strcmp(kind, AXIS_WORKER_KIND_CONVERT) == 0) {
		self->convert = read_convert_result(result, problem);
		if (self->convert == NULL)
			return NULL;
	} else if (strcmp(kind, AXIS_WORKER_KIND_VERIFY) == 0) {
		self->verify = read_result(result, problem);
		if (self->verify == NULL)
			return NULL;
	} else {
		refuse_member(problem, "kind", "is not a kind of work this protocol has");
		return NULL;
	}
	self->kind = g_strdup(kind);
	return g_steal_pointer(&self);
}

gchar *
axis_worker_completed_to_json(const AxisWorkerCompleted *self)
{
	Writer w;

	w_begin_message(&w);
	w_key(&w, "recorded");
	w_uint(&w, self->recorded);
	w_key(&w, "refused");
	w_uint(&w, self->refused);
	w_close(&w, '}');
	return w_finish(&w);
}

gboolean
axis_worker_completed_parse(
	const gchar          *data,
	gsize                 length,
	AxisWorkerCompleted  *out,
	gchar               **problem
){
	static const gchar *const members[] = { "protocol", "recorded", "refused", NULL };
	g_autoptr(AxisJsonDoc) doc = read_doc(data, length, AXIS_WORKER_MAX_RESPONSE_BYTES, problem);
	const AxisJsonNode *root;

	memset(out, 0, sizeof(*out));
	if (doc == NULL)
		return FALSE;
	root = axis_json_doc_get_root(doc);
	return only_members(root, "the complete answer", members, problem) && check_protocol(root, problem) &&
		get_uint(root, "recorded", 0, AXIS_SAMPLE_MAX_FILES, TRUE, &out->recorded, problem) &&
		get_uint(root, "refused", 0, AXIS_SAMPLE_MAX_FILES, TRUE, &out->refused, problem);
}

/* ── fail ───────────────────────────────────────────────────────── */

void
axis_worker_fail_free(AxisWorkerFail *self)
{
	if (self == NULL)
		return;
	g_free(self->lease_token);
	g_free(self->error);
	g_free(self);
}

gchar *
axis_worker_fail_to_json(const AxisWorkerFail *self)
{
	g_autofree gchar *clean = axis_library_text_clean(self->error != NULL && self->error[0] != '\0' ? self->error :
		"failed", AXIS_WORKER_ERROR_MAX_CHARS);
	Writer w;

	w_begin_message(&w);
	w_key(&w, "job");
	w_int(&w, self->job);
	w_key(&w, "lease_token");
	w_string(&w, self->lease_token);
	w_key(&w, "error");
	w_string(&w, clean != NULL && clean[0] != '\0' ? clean : "failed");
	w_key(&w, "retry");
	w_bool(&w, self->retry || self->stopping);
	w_key(&w, "stopping");
	w_bool(&w, self->stopping);
	w_close(&w, '}');
	return w_finish(&w);
}

AxisWorkerFail *
axis_worker_fail_parse(
	const gchar  *data,
	gsize         length,
	gchar       **problem
){
	static const gchar *const members[] = { "protocol", "job", "lease_token", "error", "retry", "stopping", NULL };
	g_autoptr(AxisJsonDoc) doc = read_doc(data, length, AXIS_WORKER_MAX_REQUEST_BYTES, problem);
	g_autoptr(AxisWorkerFail) self = NULL;
	const AxisJsonNode *root;

	if (doc == NULL)
		return NULL;
	root = axis_json_doc_get_root(doc);
	self = g_new0(AxisWorkerFail, 1);
	if (!only_members(root, "the fail request", members, problem) || !check_protocol(root, problem) ||
	    !get_lease(root, &self->job, &self->lease_token, problem) ||
	    !get_text(root, "error", AXIS_WORKER_ERROR_MAX_CHARS, TRUE, FALSE, &self->error, problem) ||
	    !get_bool(root, "retry", TRUE, &self->retry, problem) ||
	    !get_bool(root, "stopping", TRUE, &self->stopping, problem))
		return NULL;
	if (self->stopping && !self->retry) {
		refuse(problem, "a worker that is stopping gives the job back for another try");
		return NULL;
	}
	return g_steal_pointer(&self);
}

static gboolean
state_is_answer(const gchar *state)
{
	return g_strcmp0(state, "queued") == 0 || g_strcmp0(state, "failed") == 0 ||
		g_strcmp0(state, "cancelled") == 0;
}

gchar *
axis_worker_failed_to_json(const gchar *state)
{
	Writer w;

	w_begin_message(&w);
	w_key(&w, "state");
	w_string(&w, state_is_answer(state) ? state : "failed");
	w_close(&w, '}');
	return w_finish(&w);
}

gboolean
axis_worker_failed_parse(
	const gchar  *data,
	gsize         length,
	gchar       **state,
	gchar       **problem
){
	static const gchar *const members[] = { "protocol", "state", NULL };
	g_autoptr(AxisJsonDoc) doc = read_doc(data, length, AXIS_WORKER_MAX_RESPONSE_BYTES, problem);
	const AxisJsonNode *root;
	const gchar *text = NULL;

	*state = NULL;
	if (doc == NULL)
		return FALSE;
	root = axis_json_doc_get_root(doc);
	if (!only_members(root, "the fail answer", members, problem) || !check_protocol(root, problem) ||
	    !get_string(root, "state", 16, TRUE, FALSE, &text, problem))
		return FALSE;
	if (!state_is_answer(text))
		return refuse_member(problem, "state", "is not a state a failure can leave");
	*state = g_strdup(text);
	return TRUE;
}

/* ── Errors ─────────────────────────────────────────────────────── */

static gboolean
error_word_is_valid(const gchar *word)
{
	static const gchar *const words[] = {
		"malformed", "unauthorized", "throttled", "forbidden", "not-found", "method", "content-type",
		"too-large", "lease", "unregistered", "busy", "internal", NULL
	};

	return word != NULL && g_strv_contains(words, word);
}

gchar *
axis_worker_error_to_json_version(
	guint        version,
	const gchar *word,
	const gchar *message
){
	g_autofree gchar *clean = axis_library_text_clean(message != NULL && message[0] != '\0' ? message : "refused",
		AXIS_WORKER_ANSWER_MAX_CHARS);
	Writer w;

	/* The shape is the same in every version this side knows. */
	if (version < AXIS_WORKER_PROTOCOL_OLDEST || version > AXIS_WORKER_PROTOCOL)
		version = AXIS_WORKER_PROTOCOL;
	w_begin_version(&w, version);
	w_key(&w, "error");
	w_string(&w, error_word_is_valid(word) ? word : "internal");
	w_key(&w, "message");
	w_string(&w, clean != NULL && clean[0] != '\0' ? clean : "refused");
	w_close(&w, '}');
	return w_finish(&w);
}

gchar *
axis_worker_error_to_json(
	const gchar *word,
	const gchar *message
){
	return axis_worker_error_to_json_version(AXIS_WORKER_PROTOCOL, word, message);
}

gboolean
axis_worker_error_parse(
	const gchar  *data,
	gsize         length,
	gchar       **word,
	gchar       **message,
	guint        *version,
	gchar       **problem
){
	static const gchar *const members[] = { "protocol", "error", "message", NULL };
	g_autoptr(AxisJsonDoc) doc = read_doc(data, length, AXIS_WORKER_MAX_RESPONSE_BYTES, problem);
	const AxisJsonNode *root;
	const gchar *text = NULL;
	guint seen = 0;

	*word = NULL;
	*message = NULL;
	if (version != NULL)
		*version = 0;
	if (doc == NULL)
		return FALSE;
	root = axis_json_doc_get_root(doc);
	if (!only_members(root, "the error answer", members, problem) ||
	    !check_version(root, AXIS_WORKER_PROTOCOL_OLDEST, AXIS_WORKER_PROTOCOL, &seen, problem) ||
	    !get_string(root, "error", 16, TRUE, FALSE, &text, problem))
		return FALSE;
	if (!error_word_is_valid(text))
		return refuse_member(problem, "error", "is not an error this protocol has");
	if (!get_text(root, "message", AXIS_WORKER_ANSWER_MAX_CHARS, TRUE, FALSE, message, problem))
		return FALSE;
	*word = g_strdup(text);
	if (version != NULL)
		*version = seen;
	return TRUE;
}

gboolean
axis_worker_message_protocol(
	const gchar *data,
	gsize        length,
	guint       *version
){
	g_autoptr(AxisJsonDoc) doc = read_doc(data, length, AXIS_WORKER_MAX_REQUEST_BYTES, NULL);
	const AxisJsonNode *root;
	gint64 value = 0;

	*version = 0;
	if (doc == NULL)
		return FALSE;
	root = axis_json_doc_get_root(doc);
	if (axis_json_type(root) != AXIS_JSON_OBJECT || axis_json_member(root, "protocol") == NULL ||
	    !get_int(root, "protocol", 0, 1000, TRUE, &value, NULL))
		return FALSE;
	*version = (guint)value;
	return TRUE;
}

gchar *
axis_worker_protocol_refusal(guint theirs)
{
	if (theirs < AXIS_WORKER_PROTOCOL)
		return g_strdup_printf("this daemon speaks worker protocol %u and this axis-worker %u: run the axis-worker of "
			"the daemon's version", AXIS_WORKER_PROTOCOL, theirs);
	return g_strdup_printf("this daemon speaks worker protocol %u and this axis-worker %u: the daemon is older; "
		"update it, or run the axis-worker of its version", AXIS_WORKER_PROTOCOL, theirs);
}
