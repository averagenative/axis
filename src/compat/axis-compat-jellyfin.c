/*
 * axis-compat-jellyfin.c - the parsers for the Jellyfin server's JSON answers
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "compat/axis-compat-jellyfin.h"

#include <string.h>

#include "compat/axis-compat-plex.h"
#include "library/axis-library-name.h"
#include "probe/axis-probe-json.h"

/* A device id longer than this (bytes) is not kept at all. */
#define MAX_RAW_KEY_BYTES 512

/* ── Field readers ──────────────────────────────────────────────── */

static gchar *
text_of(
	const AxisJsonNode *node,
	guint               max_chars
){
	const gchar *raw = axis_json_scalar_text(node);
	gchar *clean;

	if (raw == NULL)
		return NULL;
	clean = axis_library_text_clean(raw, max_chars);
	if (clean == NULL)
		return NULL;
	g_strstrip(clean);
	if (clean[0] == '\0') {
		g_free(clean);
		return NULL;
	}
	return clean;
}

/* As text_of(), but a text that would have to be cut is dropped. */
static gchar *
whole_text_of(
	const AxisJsonNode *node,
	guint               max_chars
){
	const gchar *raw = axis_json_scalar_text(node);

	if (raw == NULL || g_utf8_strlen(raw, -1) > (glong)max_chars)
		return NULL;
	return text_of(node, max_chars);
}

static gchar *
word_of(const AxisJsonNode *node)
{
	gchar *word = text_of(node, AXIS_COMPAT_MAX_TOKEN_CHARS);
	gchar *lower;

	if (word == NULL)
		return NULL;
	lower = g_ascii_strdown(word, -1);
	g_free(word);
	return lower;
}

static gint64
int_of(
	const AxisJsonNode *node,
	gint64              minimum,
	gint64              maximum
){
	gint64 value;

	if (node == NULL || !axis_json_get_integer(node, minimum, maximum, &value))
		return -1;
	return value;
}

gchar *
axis_compat_jellyfin_key(const gchar *raw)
{
	const guchar *p;
	g_autofree gchar *digest = NULL;

	if (raw == NULL || raw[0] == '\0')
		return NULL;
	if (axis_compat_plex_id_valid(raw))
		return g_strdup(raw);
	if (strlen(raw) > MAX_RAW_KEY_BYTES || !g_utf8_validate(raw, -1, NULL))
		return NULL;
	for (p = (const guchar *)raw; *p != '\0'; p++) {
		if (*p < 0x20 || *p == 0x7f)
			return NULL;
	}
	digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256, raw, -1);
	return g_strdup_printf("h:%.40s", digest);
}

static gchar *
key_of(const AxisJsonNode *node)
{
	return axis_compat_jellyfin_key(axis_json_scalar_text(node));
}

static void
set_problem(
	gchar      **problem,
	const gchar *text
){
	if (problem != NULL)
		*problem = g_strdup(text);
}

/* Parses @data and checks the top value is of @type. */
static const AxisJsonNode *
open_document(
	const gchar  *data,
	gsize         length,
	AxisJsonType  type,
	AxisJsonDoc **doc,
	gchar       **problem
){
	const AxisJsonNode *root;

	*doc = axis_json_parse(data, length, problem);
	if (*doc == NULL)
		return NULL;
	root = axis_json_doc_get_root(*doc);
	if (axis_json_type(root) != type) {
		g_clear_pointer(doc, axis_json_doc_free);
		set_problem(problem, type == AXIS_JSON_ARRAY ? "the answer is not a list" :
			"the answer is not from a Jellyfin server");
		return NULL;
	}
	return root;
}

static const AxisJsonNode *
list_of(
	const AxisJsonNode *object,
	const gchar        *name
){
	const AxisJsonNode *list = axis_json_member(object, name);

	return axis_json_type(list) == AXIS_JSON_ARRAY ? list : NULL;
}

/* ── Identity, libraries, devices ───────────────────────────────── */

gboolean
axis_compat_jellyfin_parse_identity(
	const gchar        *data,
	gsize               length,
	AxisCompatIdentity *identity,
	gchar             **problem
){
	g_autoptr(AxisJsonDoc) doc = NULL;
	const AxisJsonNode *root = open_document(data, length, AXIS_JSON_OBJECT, &doc, problem);

	if (root == NULL)
		return FALSE;
	axis_compat_identity_clear(identity);
	identity->machine_id = key_of(axis_json_member(root, "Id"));
	identity->version = text_of(axis_json_member(root, "Version"), AXIS_COMPAT_MAX_TOKEN_CHARS);
	identity->name = text_of(axis_json_member(root, "ServerName"), AXIS_COMPAT_MAX_NAME_CHARS);
	return TRUE;
}

static gchar *
library_kind(const AxisJsonNode *node)
{
	g_autofree gchar *word = word_of(node);

	if (g_strcmp0(word, "movies") == 0)
		return g_strdup("movie");
	if (g_strcmp0(word, "tvshows") == 0)
		return g_strdup("show");
	if (g_strcmp0(word, "music") == 0)
		return g_strdup("artist");
	return g_strdup("other");
}

GPtrArray *
axis_compat_jellyfin_parse_libraries(
	const gchar  *data,
	gsize         length,
	gchar       **problem
){
	g_autoptr(AxisJsonDoc) doc = NULL;
	const AxisJsonNode *root = open_document(data, length, AXIS_JSON_ARRAY, &doc, problem);
	GPtrArray *libraries;
	guint i;

	if (root == NULL)
		return NULL;
	libraries = g_ptr_array_new_with_free_func((GDestroyNotify)axis_compat_library_free);
	for (i = 0; i < axis_json_length(root) && libraries->len < AXIS_COMPAT_MAX_LIBRARIES; i++) {
		const AxisJsonNode *entry = axis_json_element(root, i);
		const AxisJsonNode *locations;
		AxisCompatLibrary *library;
		guint j;

		if (axis_json_type(entry) != AXIS_JSON_OBJECT)
			continue;
		library = g_new0(AxisCompatLibrary, 1);
		library->key = key_of(axis_json_member(entry, "ItemId"));
		if (library->key == NULL) {
			/* A library that cannot be told apart cannot be used. */
			g_free(library);
			continue;
		}
		/* A mixed-content library states no collection type: "other". */
		library->kind = library_kind(axis_json_member(entry, "CollectionType"));
		library->title = text_of(axis_json_member(entry, "Name"), AXIS_COMPAT_MAX_NAME_CHARS);
		library->folders = g_ptr_array_new_with_free_func(g_free);
		locations = list_of(entry, "Locations");
		for (j = 0; locations != NULL && j < axis_json_length(locations) &&
		     library->folders->len < AXIS_COMPAT_MAX_FOLDERS; j++) {
			gchar *path = whole_text_of(axis_json_element(locations, j), AXIS_COMPAT_MAX_PATH_CHARS);

			if (path != NULL)
				g_ptr_array_add(library->folders, path);
		}
		g_ptr_array_add(libraries, library);
	}
	return libraries;
}

GPtrArray *
axis_compat_jellyfin_parse_devices(
	const gchar  *data,
	gsize         length,
	gchar       **problem
){
	g_autoptr(AxisJsonDoc) doc = NULL;
	const AxisJsonNode *root = open_document(data, length, AXIS_JSON_OBJECT, &doc, problem);
	const AxisJsonNode *list;
	GPtrArray *devices;
	guint i;

	if (root == NULL)
		return NULL;
	devices = g_ptr_array_new_with_free_func((GDestroyNotify)axis_compat_device_free);
	list = list_of(root, "Items");
	for (i = 0; list != NULL && i < axis_json_length(list) && devices->len < AXIS_COMPAT_MAX_DEVICES; i++) {
		const AxisJsonNode *entry = axis_json_element(list, i);
		AxisCompatDevice *device;

		if (axis_json_type(entry) != AXIS_JSON_OBJECT)
			continue;
		device = g_new0(AxisCompatDevice, 1);
		device->plex_id = -1;
		device->client_id = key_of(axis_json_member(entry, "Id"));
		device->name = text_of(axis_json_member(entry, "Name"), AXIS_COMPAT_MAX_NAME_CHARS);
		device->product = text_of(axis_json_member(entry, "AppName"), AXIS_COMPAT_MAX_TOKEN_CHARS);
		if (device->client_id == NULL) {
			axis_compat_device_free(device);
			continue;
		}
		g_ptr_array_add(devices, device);
	}
	return devices;
}

/* ── Sessions ───────────────────────────────────────────────────── */

static AxisCompatTri
hdr_of(const AxisJsonNode *node)
{
	g_autofree gchar *word = word_of(node);

	if (word == NULL || strcmp(word, "unknown") == 0)
		return AXIS_COMPAT_UNSTATED;
	if (strcmp(word, "sdr") == 0)
		return AXIS_COMPAT_NO;
	if (strcmp(word, "hdr10") == 0 || strcmp(word, "hdr10plus") == 0 || strcmp(word, "hlg") == 0 ||
	    strcmp(word, "dovi") == 0 || strcmp(word, "doviwithhdr10") == 0 || strcmp(word, "doviwithhlg") == 0 ||
	    strcmp(word, "doviwithhdr10plus") == 0)
		return AXIS_COMPAT_YES;
	/* Anything else (Dolby Vision over an SDR base, a new word) is not stated. */
	return AXIS_COMPAT_UNSTATED;
}

static gint
stream_type_of(const AxisJsonNode *node)
{
	g_autofree gchar *word = word_of(node);

	if (g_strcmp0(word, "video") == 0)
		return AXIS_COMPAT_STREAM_VIDEO;
	if (g_strcmp0(word, "audio") == 0)
		return AXIS_COMPAT_STREAM_AUDIO;
	if (g_strcmp0(word, "subtitle") == 0)
		return AXIS_COMPAT_STREAM_SUBTITLE;
	return 0;
}

static AxisCompatStream *
stream_from(
	const AxisJsonNode *entry,
	gint64              audio_index,
	gint64              subtitle_index
){
	AxisCompatStream *stream = g_new0(AxisCompatStream, 1);
	gint64 index = int_of(axis_json_member(entry, "Index"), 0, 100000);
	gint64 bits = int_of(axis_json_member(entry, "BitRate"), 0, G_GINT64_CONSTANT(1000000000000));

	stream->type = stream_type_of(axis_json_member(entry, "Type"));
	stream->decision = AXIS_COMPAT_DECISION_NONE;
	stream->codec = word_of(axis_json_member(entry, "Codec"));
	stream->profile = word_of(axis_json_member(entry, "Profile"));
	stream->bit_depth = (gint)int_of(axis_json_member(entry, "BitDepth"), 1, 64);
	stream->width = (gint)int_of(axis_json_member(entry, "Width"), 1, 32768);
	stream->height = (gint)int_of(axis_json_member(entry, "Height"), 1, 32768);
	stream->channels = (gint)int_of(axis_json_member(entry, "Channels"), 1, 256);
	stream->bitrate_kbps = bits >= 0 ? bits / 1000 : -1;
	stream->selected = AXIS_COMPAT_UNSTATED;
	/* Which audio or subtitle track plays is known only when the client
	 * reported it. */
	if (stream->type == AXIS_COMPAT_STREAM_AUDIO && audio_index >= 0 && index >= 0)
		stream->selected = index == audio_index ? AXIS_COMPAT_YES : AXIS_COMPAT_NO;
	else if (stream->type == AXIS_COMPAT_STREAM_SUBTITLE && subtitle_index >= 0 && index >= 0)
		stream->selected = index == subtitle_index ? AXIS_COMPAT_YES : AXIS_COMPAT_NO;
	stream->hdr = stream->type == AXIS_COMPAT_STREAM_VIDEO ? hdr_of(axis_json_member(entry, "VideoRangeType")) :
		AXIS_COMPAT_UNSTATED;
	return stream;
}

static AxisCompatDecision
play_method_of(const AxisJsonNode *node)
{
	g_autofree gchar *word = word_of(node);

	if (word == NULL)
		return AXIS_COMPAT_DECISION_NONE;
	if (strcmp(word, "directplay") == 0)
		return AXIS_COMPAT_DECISION_DIRECT;
	if (strcmp(word, "directstream") == 0)
		return AXIS_COMPAT_DECISION_COPY;
	if (strcmp(word, "transcode") == 0)
		return AXIS_COMPAT_DECISION_TRANSCODE;
	return AXIS_COMPAT_DECISION_UNKNOWN;
}

static AxisCompatDecision
direct_of(const AxisJsonNode *node)
{
	gboolean direct;

	if (node == NULL || !axis_json_get_boolean(node, &direct))
		return AXIS_COMPAT_DECISION_NONE;
	return direct ? AXIS_COMPAT_DECISION_COPY : AXIS_COMPAT_DECISION_TRANSCODE;
}

static void
read_transcode(
	const AxisJsonNode  *node,
	AxisCompatTranscode *tc
){
	const AxisJsonNode *list;
	guint i;

	if (axis_json_type(node) != AXIS_JSON_OBJECT)
		return;
	tc->present = TRUE;
	tc->video_decision = direct_of(axis_json_member(node, "IsVideoDirect"));
	tc->audio_decision = direct_of(axis_json_member(node, "IsAudioDirect"));
	tc->video_codec = word_of(axis_json_member(node, "VideoCodec"));
	tc->audio_codec = word_of(axis_json_member(node, "AudioCodec"));
	tc->container = word_of(axis_json_member(node, "Container"));
	tc->width = (gint)int_of(axis_json_member(node, "Width"), 1, 32768);
	tc->height = (gint)int_of(axis_json_member(node, "Height"), 1, 32768);
	list = list_of(node, "TranscodeReasons");
	if (list == NULL)
		return;
	tc->reasons = g_ptr_array_new_with_free_func(g_free);
	for (i = 0; i < axis_json_length(list) && tc->reasons->len < AXIS_COMPAT_MAX_REASONS; i++) {
		gchar *reason = text_of(axis_json_element(list, i), AXIS_COMPAT_MAX_REASON_CHARS);

		if (reason != NULL)
			g_ptr_array_add(tc->reasons, reason);
	}
	if (tc->reasons->len == 0)
		g_clear_pointer(&tc->reasons, g_ptr_array_unref);
	/* The picture is converted when a subtitle has to be burned in: the
	 * server says so with a reason, and that is the only way it says the
	 * subtitle stream was converted. */
	for (i = 0; tc->reasons != NULL && i < tc->reasons->len; i++) {
		if (strcmp(g_ptr_array_index(tc->reasons, i), "SubtitleCodecNotSupported") == 0)
			tc->subtitle_decision = AXIS_COMPAT_DECISION_TRANSCODE;
	}
}

static AxisCompatSession *
session_from(
	const AxisJsonNode *entry,
	const AxisJsonNode *item
){
	AxisCompatSession *session = g_new0(AxisCompatSession, 1);
	const AxisJsonNode *state = axis_json_member(entry, "PlayState");
	const AxisJsonNode *streams = list_of(item, "MediaStreams");
	g_autofree gchar *item_id = key_of(axis_json_member(item, "Id"));
	g_autofree gchar *source_id = key_of(axis_json_member(state, "MediaSourceId"));
	gint64 audio_index = int_of(axis_json_member(state, "AudioStreamIndex"), 0, 100000);
	gint64 subtitle_index = int_of(axis_json_member(state, "SubtitleStreamIndex"), 0, 100000);
	guint i;

	session->streams = g_ptr_array_new_with_free_func((GDestroyNotify)axis_compat_stream_free);
	session->media_bitrate_kbps = -1;
	session->bandwidth_kbps = -1;
	session->player_local = AXIS_COMPAT_UNSTATED;
	session->session_key = key_of(axis_json_member(entry, "Id"));
	session->rating_key = g_strdup(item_id);
	session->item_type = word_of(axis_json_member(item, "Type"));
	session->device_client_id = key_of(axis_json_member(entry, "DeviceId"));
	session->device_name = text_of(axis_json_member(entry, "DeviceName"), AXIS_COMPAT_MAX_NAME_CHARS);
	session->product = text_of(axis_json_member(entry, "Client"), AXIS_COMPAT_MAX_TOKEN_CHARS);
	session->account_id = key_of(axis_json_member(entry, "UserId"));
	session->account_name = text_of(axis_json_member(entry, "UserName"), AXIS_COMPAT_MAX_NAME_CHARS);
	session->part_decision = play_method_of(axis_json_member(state, "PlayMethod"));
	session->container = word_of(axis_json_member(item, "Container"));
	session->file = whole_text_of(axis_json_member(item, "Path"), AXIS_COMPAT_MAX_PATH_CHARS);
	read_transcode(axis_json_member(entry, "TranscodingInfo"), &session->transcode);
	/* The streams are those of the item's own file: if the client played
	 * another version of it, they say nothing about what played. */
	if (source_id != NULL && item_id != NULL && strcmp(source_id, item_id) != 0)
		session->ambiguous = TRUE;
	for (i = 0; streams != NULL && i < axis_json_length(streams) && session->streams->len < AXIS_COMPAT_MAX_STREAMS;
	     i++) {
		const AxisJsonNode *s = axis_json_element(streams, i);

		if (axis_json_type(s) == AXIS_JSON_OBJECT)
			g_ptr_array_add(session->streams, stream_from(s, audio_index, subtitle_index));
	}
	return session;
}

GPtrArray *
axis_compat_jellyfin_parse_sessions(
	const gchar  *data,
	gsize         length,
	gchar       **problem
){
	g_autoptr(AxisJsonDoc) doc = NULL;
	const AxisJsonNode *root = open_document(data, length, AXIS_JSON_ARRAY, &doc, problem);
	GPtrArray *sessions;
	guint i;

	if (root == NULL)
		return NULL;
	sessions = g_ptr_array_new_with_free_func((GDestroyNotify)axis_compat_session_free);
	for (i = 0; i < axis_json_length(root) && sessions->len < AXIS_COMPAT_MAX_SESSIONS; i++) {
		const AxisJsonNode *entry = axis_json_element(root, i);
		const AxisJsonNode *item;

		if (axis_json_type(entry) != AXIS_JSON_OBJECT)
			continue;
		item = axis_json_member(entry, "NowPlayingItem");
		/* A connected client that plays nothing is not a play. */
		if (axis_json_type(item) != AXIS_JSON_OBJECT)
			continue;
		g_ptr_array_add(sessions, session_from(entry, item));
	}
	return sessions;
}

/* ── Item ───────────────────────────────────────────────────────── */

gboolean
axis_compat_jellyfin_parse_item(
	const gchar    *data,
	gsize           length,
	AxisCompatItem *item,
	gchar         **problem
){
	g_autoptr(AxisJsonDoc) doc = NULL;
	const AxisJsonNode *root = open_document(data, length, AXIS_JSON_OBJECT, &doc, problem);
	const AxisJsonNode *list;

	if (root == NULL)
		return FALSE;
	axis_compat_item_clear(item);
	item->n_parts = 0;
	list = list_of(root, "Items");
	/* More than one item for one id cannot be (and which is not known). */
	if (list == NULL || axis_json_length(list) != 1)
		return TRUE;
	item->n_parts = 1;
	item->file = whole_text_of(axis_json_member(axis_json_element(list, 0), "Path"), AXIS_COMPAT_MAX_PATH_CHARS);
	return TRUE;
}
