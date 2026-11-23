/*
 * axis-compat-plex.c - the parsers for the Plex Media Server's JSON answers
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "compat/axis-compat-plex.h"

#include <string.h>

#include "library/axis-library-name.h"
#include "probe/axis-probe-json.h"

/* ── Field readers ──────────────────────────────────────────────── */

/* The text of a string or number, cleaned, stripped and cut; NULL if there
 * is none. */
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

gboolean
axis_compat_plex_id_valid(const gchar *id)
{
	const gchar *p;

	if (id == NULL || id[0] == '\0' || strlen(id) > AXIS_COMPAT_MAX_ID_CHARS)
		return FALSE;
	/* An id becomes part of a request path ("/library/metadata/<id>"): it
	 * must never be able to climb out of it. */
	if (strstr(id, "..") != NULL)
		return FALSE;
	for (p = id; *p != '\0'; p++) {
		if (!g_ascii_isalnum(*p) && *p != '_' && *p != '.' && *p != ':' && *p != '-')
			return FALSE;
	}
	return TRUE;
}

static gchar *
id_of(const AxisJsonNode *node)
{
	const gchar *raw = axis_json_scalar_text(node);

	if (raw == NULL || !axis_compat_plex_id_valid(raw))
		return NULL;
	return g_strdup(raw);
}

/* A short lower-case word (a codec, a profile, a platform). */
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

static AxisCompatTri
tri_of(const AxisJsonNode *node)
{
	gboolean value;

	if (node == NULL || !axis_json_get_boolean(node, &value))
		return AXIS_COMPAT_UNSTATED;
	return value ? AXIS_COMPAT_YES : AXIS_COMPAT_NO;
}

static AxisCompatDecision
decision_of(const AxisJsonNode *node)
{
	const gchar *raw = axis_json_string(node);

	if (raw == NULL)
		return node == NULL || axis_json_type(node) == AXIS_JSON_NULL ?
			AXIS_COMPAT_DECISION_NONE : AXIS_COMPAT_DECISION_UNKNOWN;
	return axis_compat_decision_from_string(raw);
}

/* ── The container ──────────────────────────────────────────────── */

static void
set_problem(
	gchar      **problem,
	const gchar *text
){
	if (problem != NULL)
		*problem = g_strdup(text);
}

/* Parses @data and finds its MediaContainer; the document must outlive the
 * node. */
static const AxisJsonNode *
open_container(
	const gchar  *data,
	gsize         length,
	AxisJsonDoc **doc,
	gchar       **problem
){
	const AxisJsonNode *root;
	const AxisJsonNode *container;

	*doc = axis_json_parse(data, length, problem);
	if (*doc == NULL)
		return NULL;
	root = axis_json_doc_get_root(*doc);
	container = axis_json_member(root, "MediaContainer");
	if (axis_json_type(container) != AXIS_JSON_OBJECT) {
		g_clear_pointer(doc, axis_json_doc_free);
		set_problem(problem, "the answer is not a media container");
		return NULL;
	}
	return container;
}

/* The elements of the array member @name; none if it is absent or not an
 * array. */
static const AxisJsonNode *
list_of(
	const AxisJsonNode *container,
	const gchar        *name
){
	const AxisJsonNode *list = axis_json_member(container, name);

	return axis_json_type(list) == AXIS_JSON_ARRAY ? list : NULL;
}

/* ── Identity, libraries, devices, accounts ─────────────────────── */

gboolean
axis_compat_plex_parse_identity(
	const gchar        *data,
	gsize               length,
	AxisCompatIdentity *identity,
	gchar             **problem
){
	g_autoptr(AxisJsonDoc) doc = NULL;
	const AxisJsonNode *container = open_container(data, length, &doc, problem);

	if (container == NULL)
		return FALSE;
	axis_compat_identity_clear(identity);
	identity->machine_id = id_of(axis_json_member(container, "machineIdentifier"));
	identity->version = text_of(axis_json_member(container, "version"), AXIS_COMPAT_MAX_TOKEN_CHARS);
	return TRUE;
}

gboolean
axis_compat_plex_parse_root(
	const gchar        *data,
	gsize               length,
	AxisCompatIdentity *identity,
	gchar             **problem
){
	g_autoptr(AxisJsonDoc) doc = NULL;
	const AxisJsonNode *container = open_container(data, length, &doc, problem);

	if (container == NULL)
		return FALSE;
	g_clear_pointer(&identity->name, g_free);
	identity->name = text_of(axis_json_member(container, "friendlyName"), AXIS_COMPAT_MAX_NAME_CHARS);
	return TRUE;
}

static gchar *
library_kind(const AxisJsonNode *node)
{
	g_autofree gchar *word = word_of(node);

	if (g_strcmp0(word, "movie") == 0 || g_strcmp0(word, "show") == 0 || g_strcmp0(word, "artist") == 0)
		return g_steal_pointer(&word);
	return g_strdup("other");
}

GPtrArray *
axis_compat_plex_parse_libraries(
	const gchar  *data,
	gsize         length,
	gchar       **problem
){
	g_autoptr(AxisJsonDoc) doc = NULL;
	const AxisJsonNode *container = open_container(data, length, &doc, problem);
	const AxisJsonNode *list;
	GPtrArray *libraries;
	guint i;

	if (container == NULL)
		return NULL;
	libraries = g_ptr_array_new_with_free_func((GDestroyNotify)axis_compat_library_free);
	list = list_of(container, "Directory");
	for (i = 0; list != NULL && i < axis_json_length(list) && libraries->len < AXIS_COMPAT_MAX_LIBRARIES; i++) {
		const AxisJsonNode *entry = axis_json_element(list, i);
		const AxisJsonNode *locations = list_of(entry, "Location");
		AxisCompatLibrary *library;
		guint j;

		if (axis_json_type(entry) != AXIS_JSON_OBJECT)
			continue;
		library = g_new0(AxisCompatLibrary, 1);
		library->key = id_of(axis_json_member(entry, "key"));
		if (library->key == NULL) {
			/* A library that cannot be told apart cannot be used. */
			g_free(library);
			continue;
		}
		library->kind = library_kind(axis_json_member(entry, "type"));
		library->title = text_of(axis_json_member(entry, "title"), AXIS_COMPAT_MAX_NAME_CHARS);
		library->folders = g_ptr_array_new_with_free_func(g_free);
		for (j = 0; locations != NULL && j < axis_json_length(locations) &&
		     library->folders->len < AXIS_COMPAT_MAX_FOLDERS; j++) {
			gchar *path = whole_text_of(axis_json_member(axis_json_element(locations, j), "path"),
				AXIS_COMPAT_MAX_PATH_CHARS);

			if (path != NULL)
				g_ptr_array_add(library->folders, path);
		}
		g_ptr_array_add(libraries, library);
	}
	return libraries;
}

GPtrArray *
axis_compat_plex_parse_devices(
	const gchar  *data,
	gsize         length,
	gchar       **problem
){
	g_autoptr(AxisJsonDoc) doc = NULL;
	const AxisJsonNode *container = open_container(data, length, &doc, problem);
	const AxisJsonNode *list;
	GPtrArray *devices;
	guint i;

	if (container == NULL)
		return NULL;
	devices = g_ptr_array_new_with_free_func((GDestroyNotify)axis_compat_device_free);
	list = list_of(container, "Device");
	for (i = 0; list != NULL && i < axis_json_length(list) && devices->len < AXIS_COMPAT_MAX_DEVICES; i++) {
		const AxisJsonNode *entry = axis_json_element(list, i);
		AxisCompatDevice *device;

		if (axis_json_type(entry) != AXIS_JSON_OBJECT)
			continue;
		device = g_new0(AxisCompatDevice, 1);
		device->plex_id = int_of(axis_json_member(entry, "id"), 0, G_GINT64_CONSTANT(9007199254740991));
		device->client_id = id_of(axis_json_member(entry, "clientIdentifier"));
		device->name = text_of(axis_json_member(entry, "name"), AXIS_COMPAT_MAX_NAME_CHARS);
		device->platform = text_of(axis_json_member(entry, "platform"), AXIS_COMPAT_MAX_TOKEN_CHARS);
		if (device->plex_id < 0 && device->client_id == NULL) {
			axis_compat_device_free(device);
			continue;
		}
		g_ptr_array_add(devices, device);
	}
	return devices;
}

GPtrArray *
axis_compat_plex_parse_accounts(
	const gchar  *data,
	gsize         length,
	gchar       **problem
){
	g_autoptr(AxisJsonDoc) doc = NULL;
	const AxisJsonNode *container = open_container(data, length, &doc, problem);
	const AxisJsonNode *list;
	GPtrArray *accounts;
	guint i;

	if (container == NULL)
		return NULL;
	accounts = g_ptr_array_new_with_free_func((GDestroyNotify)axis_compat_account_free);
	list = list_of(container, "Account");
	for (i = 0; list != NULL && i < axis_json_length(list) && accounts->len < AXIS_COMPAT_MAX_ACCOUNTS; i++) {
		const AxisJsonNode *entry = axis_json_element(list, i);
		AxisCompatAccount *account;

		if (axis_json_type(entry) != AXIS_JSON_OBJECT)
			continue;
		account = g_new0(AxisCompatAccount, 1);
		account->id = id_of(axis_json_member(entry, "id"));
		account->name = text_of(axis_json_member(entry, "name"), AXIS_COMPAT_MAX_NAME_CHARS);
		if (account->id == NULL) {
			axis_compat_account_free(account);
			continue;
		}
		g_ptr_array_add(accounts, account);
	}
	return accounts;
}

/* ── Sessions ───────────────────────────────────────────────────── */

static gboolean
colour_is_hdr(const gchar *transfer)
{
	return strcmp(transfer, "smpte2084") == 0 || strcmp(transfer, "arib-std-b67") == 0;
}

static AxisCompatStream *
stream_from(const AxisJsonNode *entry)
{
	AxisCompatStream *stream = g_new0(AxisCompatStream, 1);
	g_autofree gchar *transfer = NULL;

	stream->type = (gint)int_of(axis_json_member(entry, "streamType"), 1, 3);
	if (stream->type < 0)
		stream->type = 0;
	stream->decision = decision_of(axis_json_member(entry, "decision"));
	stream->codec = word_of(axis_json_member(entry, "codec"));
	stream->profile = word_of(axis_json_member(entry, "profile"));
	stream->bit_depth = (gint)int_of(axis_json_member(entry, "bitDepth"), 1, 64);
	stream->width = (gint)int_of(axis_json_member(entry, "width"), 1, 32768);
	stream->height = (gint)int_of(axis_json_member(entry, "height"), 1, 32768);
	stream->channels = (gint)int_of(axis_json_member(entry, "channels"), 1, 256);
	stream->bitrate_kbps = int_of(axis_json_member(entry, "bitrate"), 0, 1000000000);
	stream->selected = tri_of(axis_json_member(entry, "selected"));
	transfer = word_of(axis_json_member(entry, "colorTrc"));
	stream->hdr = transfer == NULL ? AXIS_COMPAT_UNSTATED :
		colour_is_hdr(transfer) ? AXIS_COMPAT_YES : AXIS_COMPAT_NO;
	return stream;
}

static void
read_transcode(
	const AxisJsonNode  *node,
	AxisCompatTranscode *tc
){
	if (axis_json_type(node) != AXIS_JSON_OBJECT)
		return;
	tc->present = TRUE;
	tc->video_decision = decision_of(axis_json_member(node, "videoDecision"));
	tc->audio_decision = decision_of(axis_json_member(node, "audioDecision"));
	tc->subtitle_decision = decision_of(axis_json_member(node, "subtitleDecision"));
	tc->source_video_codec = word_of(axis_json_member(node, "sourceVideoCodec"));
	tc->source_audio_codec = word_of(axis_json_member(node, "sourceAudioCodec"));
	tc->video_codec = word_of(axis_json_member(node, "videoCodec"));
	tc->audio_codec = word_of(axis_json_member(node, "audioCodec"));
	tc->container = word_of(axis_json_member(node, "container"));
	tc->protocol = word_of(axis_json_member(node, "protocol"));
	tc->width = (gint)int_of(axis_json_member(node, "width"), 1, 32768);
	tc->height = (gint)int_of(axis_json_member(node, "height"), 1, 32768);
}

static void
read_media(
	const AxisJsonNode *entry,
	AxisCompatSession  *session
){
	const AxisJsonNode *medias = list_of(entry, "Media");
	const AxisJsonNode *media;
	const AxisJsonNode *parts;
	const AxisJsonNode *part;
	const AxisJsonNode *streams;
	guint i;

	if (medias == NULL || axis_json_length(medias) == 0)
		return;
	if (axis_json_length(medias) != 1) {
		session->ambiguous = TRUE;
		return;
	}
	media = axis_json_element(medias, 0);
	if (axis_json_type(media) != AXIS_JSON_OBJECT)
		return;
	session->media_bitrate_kbps = int_of(axis_json_member(media, "bitrate"), 0, 1000000000);
	session->container = word_of(axis_json_member(media, "container"));
	parts = list_of(media, "Part");
	if (parts == NULL || axis_json_length(parts) == 0)
		return;
	if (axis_json_length(parts) != 1) {
		session->ambiguous = TRUE;
		return;
	}
	part = axis_json_element(parts, 0);
	if (axis_json_type(part) != AXIS_JSON_OBJECT)
		return;
	session->part_decision = decision_of(axis_json_member(part, "decision"));
	{
		gchar *container = word_of(axis_json_member(part, "container"));

		if (container != NULL) {
			g_free(session->container);
			session->container = container;
		}
	}
	session->file = whole_text_of(axis_json_member(part, "file"), AXIS_COMPAT_MAX_PATH_CHARS);
	streams = list_of(part, "Stream");
	for (i = 0; streams != NULL && i < axis_json_length(streams) && session->streams->len < AXIS_COMPAT_MAX_STREAMS;
	     i++) {
		const AxisJsonNode *s = axis_json_element(streams, i);

		if (axis_json_type(s) == AXIS_JSON_OBJECT)
			g_ptr_array_add(session->streams, stream_from(s));
	}
}

static AxisCompatSession *
session_from(const AxisJsonNode *entry)
{
	AxisCompatSession *session = g_new0(AxisCompatSession, 1);
	const AxisJsonNode *player = axis_json_member(entry, "Player");
	const AxisJsonNode *info = axis_json_member(entry, "Session");
	const AxisJsonNode *user = axis_json_member(entry, "User");

	session->streams = g_ptr_array_new_with_free_func((GDestroyNotify)axis_compat_stream_free);
	session->media_bitrate_kbps = -1;
	session->bandwidth_kbps = -1;
	session->rating_key = id_of(axis_json_member(entry, "ratingKey"));
	session->library_key = id_of(axis_json_member(entry, "librarySectionID"));
	session->item_type = word_of(axis_json_member(entry, "type"));
	session->device_client_id = id_of(axis_json_member(player, "machineIdentifier"));
	session->device_name = text_of(axis_json_member(player, "title"), AXIS_COMPAT_MAX_NAME_CHARS);
	session->platform = text_of(axis_json_member(player, "platform"), AXIS_COMPAT_MAX_TOKEN_CHARS);
	session->product = text_of(axis_json_member(player, "product"), AXIS_COMPAT_MAX_TOKEN_CHARS);
	session->player_local = tri_of(axis_json_member(player, "local"));
	session->session_key = id_of(axis_json_member(info, "id"));
	session->location = word_of(axis_json_member(info, "location"));
	session->bandwidth_kbps = int_of(axis_json_member(info, "bandwidth"), 0, 1000000000);
	session->account_id = id_of(axis_json_member(user, "id"));
	session->account_name = text_of(axis_json_member(user, "title"), AXIS_COMPAT_MAX_NAME_CHARS);
	read_transcode(axis_json_member(entry, "TranscodeSession"), &session->transcode);
	read_media(entry, session);
	return session;
}

GPtrArray *
axis_compat_plex_parse_sessions(
	const gchar  *data,
	gsize         length,
	gchar       **problem
){
	g_autoptr(AxisJsonDoc) doc = NULL;
	const AxisJsonNode *container = open_container(data, length, &doc, problem);
	const AxisJsonNode *list;
	GPtrArray *sessions;
	guint i;

	if (container == NULL)
		return NULL;
	sessions = g_ptr_array_new_with_free_func((GDestroyNotify)axis_compat_session_free);
	list = list_of(container, "Metadata");
	for (i = 0; list != NULL && i < axis_json_length(list) && sessions->len < AXIS_COMPAT_MAX_SESSIONS; i++) {
		const AxisJsonNode *entry = axis_json_element(list, i);

		if (axis_json_type(entry) == AXIS_JSON_OBJECT)
			g_ptr_array_add(sessions, session_from(entry));
	}
	return sessions;
}

/* ── History and item ───────────────────────────────────────────── */

gboolean
axis_compat_plex_parse_history(
	const gchar       *data,
	gsize              length,
	AxisCompatHistory *history,
	gchar            **problem
){
	g_autoptr(AxisJsonDoc) doc = NULL;
	const AxisJsonNode *container = open_container(data, length, &doc, problem);
	const AxisJsonNode *list;
	guint i;

	if (container == NULL)
		return FALSE;
	axis_compat_history_clear(history);
	history->plays = g_ptr_array_new_with_free_func((GDestroyNotify)axis_compat_play_free);
	history->total = int_of(axis_json_member(container, "totalSize"), 0, G_GINT64_CONSTANT(1000000000000));
	list = list_of(container, "Metadata");
	for (i = 0; list != NULL && i < axis_json_length(list) && history->plays->len < AXIS_COMPAT_MAX_HISTORY; i++) {
		const AxisJsonNode *entry = axis_json_element(list, i);
		AxisCompatPlay *play;

		if (axis_json_type(entry) != AXIS_JSON_OBJECT)
			continue;
		play = g_new0(AxisCompatPlay, 1);
		play->account_id = id_of(axis_json_member(entry, "accountID"));
		play->device_id = int_of(axis_json_member(entry, "deviceID"), 0, G_GINT64_CONSTANT(9007199254740991));
		play->library_key = id_of(axis_json_member(entry, "librarySectionID"));
		play->rating_key = id_of(axis_json_member(entry, "ratingKey"));
		/* 1970 to 2200: anything else is not a time. */
		play->viewed_at = int_of(axis_json_member(entry, "viewedAt"), 0, G_GINT64_CONSTANT(7258118400));
		g_ptr_array_add(history->plays, play);
	}
	return TRUE;
}

gboolean
axis_compat_plex_parse_item(
	const gchar    *data,
	gsize           length,
	AxisCompatItem *item,
	gchar         **problem
){
	g_autoptr(AxisJsonDoc) doc = NULL;
	const AxisJsonNode *container = open_container(data, length, &doc, problem);
	const AxisJsonNode *list;
	const AxisJsonNode *medias;
	const AxisJsonNode *parts;

	if (container == NULL)
		return FALSE;
	axis_compat_item_clear(item);
	item->n_parts = 0;
	list = list_of(container, "Metadata");
	if (list == NULL || axis_json_length(list) != 1)
		return TRUE;
	medias = list_of(axis_json_element(list, 0), "Media");
	if (medias == NULL || axis_json_length(medias) != 1)
		return TRUE;
	parts = list_of(axis_json_element(medias, 0), "Part");
	if (parts == NULL)
		return TRUE;
	item->n_parts = (gint)MIN(axis_json_length(parts), 1000u);
	/* With more than one part, which file plays is not known. */
	if (axis_json_length(parts) == 1)
		item->file = whole_text_of(axis_json_member(axis_json_element(parts, 0), "file"),
			AXIS_COMPAT_MAX_PATH_CHARS);
	return TRUE;
}
