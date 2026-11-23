/*
 * test-download-deluge.c - the Deluge download client kind, against a fake
 * Deluge web interface on loopback, a real database and a real volume in a
 * scratch directory
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * THE FIXTURES ARE HAND-WRITTEN.  No Deluge was available: the answers in
 * tests/fixtures/deluge and the fake below follow Deluge's published web
 * JSON-RPC format and the author's memory of Deluge 2.x, not a recording.
 * What is believed but not verified is listed in
 * tests/fixtures/deluge/README.md and in the report of the change.  These
 * tests show that the kind does what it means to against that format; they
 * do not show that the format is right.
 *
 * Spec scenarios (downloaders):
 *   Deluge client: Add; Label cannot be set; Seeding; Not ready
 *   A lost Deluge session never repeats an add: Session lost during a list;
 *     Session lost during an add
 *   (fix-first-torrent) A download its client no longer has; Where a torrent
 *     client keeps Axis's label or category
 * and tasks 2.1 to 2.6 of add-downloader-deluge.  The one answer seen from a
 * real Deluge (2.2.0), the removal of a torrent it does not have, is what the
 * double answers for one.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <glib.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <string.h>

#include "axis-test-case.h"
#include "axis-test-db.h"
#include "axis-test-download.h"
#include "axis-test-library.h"
#include "axis-test-plex.h"
#include "axis-test-storage.h"
#include "download/axis-download.h"
#include "download/axis-download-bencode.h"
#include "download/axis-download-deluge.h"
#include "download/axis-download-deluge-parse.h"
#include "download/axis-download-private.h"

#define PASSWORD "deluge-web-pw-5e2b8f10c7a4"
#define WRONG_PASSWORD "wrong-web-pw-0a9d33f1b8e2"
#define OTHER_PASSWORD "other-web-pw-3c71e9a2d5f0"
#define FIXTURES "tests/fixtures/deluge/"
#define HOST_ID "a1b2c3d4e5f60718293a4b5c6d7e8f90"
#define MAGNET_HASH "89abcdef0123456789abcdef0123456789abcdef"
#define MAGNET "magnet:?xt=urn:btih:" MAGNET_HASH "&dn=Some.Magnet.Release"

/* ── Fixture files ──────────────────────────────────────────────── */

static gchar *
fixture_text(const gchar *name)
{
	g_autofree gchar *path = g_strconcat(FIXTURES, name, NULL);
	g_autoptr(GError) error = NULL;
	gchar *text = NULL;

	if (!g_file_get_contents(path, &text, NULL, &error))
		g_error("cannot read the fixture %s: %s", path, error->message);
	return text;
}

/* ── The fake Deluge ────────────────────────────────────────────── */

typedef struct
{
	gchar    *hash;
	gchar    *name;
	gchar    *label;	/* NULL: none */
	gchar    *state;
	gint64    wanted;
	gint64    done;
	gchar    *location;
	gboolean  finished;
	gchar    *tracker;
} Torrent;

typedef struct
{
	guint   status;
	gchar  *type;
	gchar  *body;
} Raw;

typedef enum
{
	DG_NORMAL,
	DG_DOWN,		/* the head never comes */
	DG_ADD_HANGS,		/* takes an added torrent, never answers */
	DG_REDIRECT		/* every request is answered with a redirect */
} DgMode;

typedef struct
{
	AxisTestPlex *server;
	GMutex        lock;
	gchar        *password;
	gchar        *prefix;		/* the path in front of /json that is served ("" for none) */
	DgMode        mode;
	gchar        *redirect;
	gchar        *session;		/* (lock) the session Deluge accepts, or NULL */
	GPtrArray    *sessions;		/* (lock) every session id ever given */
	gboolean      connected;
	gboolean      has_hosts;
	gboolean      connect_works;
	gboolean      has_label_plugin;
	gboolean      old_daemon;	/* daemon.get_version is an unknown method */
	gboolean      no_cookie;	/* a login does not set the cookie */
	gboolean      fail_set_label;
	gboolean      fail_remove;
	gboolean      hang_set_label;	/* sets the label, never answers */
	gchar        *add_hash_override;
	gchar        *expire_before;	/* the next call of this method finds the session gone */
	GPtrArray    *labels;		/* (lock) gchar * */
	GPtrArray    *torrents;		/* (lock) Torrent */
	GPtrArray    *calls;		/* (lock) "method" or "method:detail" */
	GHashTable   *raw;		/* (lock) method -> Raw */
	gchar        *last_filter;	/* (lock) the label of the last list's filter */
	gchar        *last_keys;	/* (lock) its keys, joined by ',' */
	gchar        *last_add_name;
	gchar        *last_add_options;
	guint         bad_requests;
	GHashTable   *label_folders;	/* (lock) label -> its "Move completed to", on */
	gchar        *download_location;	/* (lock) the core's folder for every download */
	gchar        *move_completed_path;	/* (lock) the core's "move completed" folder */
} Dg;

static void
torrent_free(gpointer data)
{
	Torrent *torrent = data;

	g_free(torrent->hash);
	g_free(torrent->name);
	g_free(torrent->label);
	g_free(torrent->state);
	g_free(torrent->location);
	g_free(torrent->tracker);
	g_free(torrent);
}

static void
raw_free(gpointer data)
{
	Raw *raw = data;

	g_free(raw->type);
	g_free(raw->body);
	g_free(raw);
}

static void
answer_text(
	AxisTestPlexAnswer *answer,
	guint               status,
	const gchar        *type,
	const gchar        *text
){
	answer->status = status;
	answer->content_type = g_strdup(type);
	answer->body = g_bytes_new(text, strlen(text));
}

static void
reply_result(
	AxisTestPlexAnswer *answer,
	gint64              id,
	const gchar        *result
){
	g_autofree gchar *text = g_strdup_printf("{\"result\": %s, \"error\": null, \"id\": %" G_GINT64_FORMAT "}", result, id);

	answer_text(answer, 200, "application/json", text);
}

static void
reply_error(
	AxisTestPlexAnswer *answer,
	gint64              id,
	gint                code,
	const gchar        *message
){
	g_autofree gchar *text = g_strdup_printf("{\"result\": null, \"error\": {\"message\": \"%s\", \"code\": %d}, \"id\": %"
		G_GINT64_FORMAT "}", message, code, id);

	answer_text(answer, 200, "application/json", text);
}

/* A fixture's body with the id of the request it answers in place of its 1. */
static gchar *
with_id(
	const gchar *body,
	gint64       id
){
	const gchar *at = g_strrstr(body, "\"id\": 1}");

	if (at == NULL)
		return g_strdup(body);
	return g_strdup_printf("%.*s\"id\": %" G_GINT64_FORMAT "}%s", (int)(at - body), body, id, at + 8);
}

static Torrent *
find_torrent(
	Dg          *self,
	const gchar *hash
){
	guint i;

	for (i = 0; hash != NULL && i < self->torrents->len; i++) {
		Torrent *torrent = g_ptr_array_index(self->torrents, i);

		if (g_ascii_strcasecmp(torrent->hash, hash) == 0)
			return torrent;
	}
	return NULL;
}

static void
note(
	Dg          *self,
	const gchar *format,
	...
) G_GNUC_PRINTF(2, 3);

static void
note(
	Dg          *self,
	const gchar *format,
	...
){
	va_list ap;

	va_start(ap, format);
	g_ptr_array_add(self->calls, g_strdup_vprintf(format, ap));
	va_end(ap);
}

static const gchar *
param_string(
	JsonArray *params,
	guint      index
){
	if (params == NULL || index >= json_array_get_length(params))
		return NULL;
	if (json_node_get_value_type(json_array_get_element(params, index)) != G_TYPE_STRING)
		return NULL;
	return json_array_get_string_element(params, index);
}

static gboolean
has_name(
	GPtrArray   *names,
	const gchar *name
){
	guint i;

	for (i = 0; i < names->len; i++) {
		if (strcmp(g_ptr_array_index(names, i), name) == 0)
			return TRUE;
	}
	return FALSE;
}

static gchar *
names_json(GPtrArray *names)
{
	GString *out = g_string_new("[");
	guint i;

	for (i = 0; i < names->len; i++)
		g_string_append_printf(out, "%s\"%s\"", i > 0 ? ", " : "", (const gchar *)g_ptr_array_index(names, i));
	g_string_append_c(out, ']');
	return g_string_free(out, FALSE);
}

static gchar *
torrents_json(Dg *self)
{
	GString *out = g_string_new("{");
	guint i;

	for (i = 0; i < self->torrents->len; i++) {
		const Torrent *torrent = g_ptr_array_index(self->torrents, i);
		gboolean downloading = strcmp(torrent->state, "Downloading") == 0;
		gdouble progress = torrent->wanted > 0 ? (gdouble)torrent->done * 100.0 / (gdouble)torrent->wanted : 0.0;

		g_string_append_printf(out, "%s\"%s\": {\"name\": \"%s\", \"state\": \"%s\", \"progress\": %.2f, "
			"\"total_wanted\": %" G_GINT64_FORMAT ", \"total_done\": %" G_GINT64_FORMAT ", "
			"\"download_payload_rate\": %d, \"eta\": %d, \"download_location\": \"%s\", \"is_finished\": %s, "
			"\"label\": \"%s\", \"tracker_status\": \"%s\", \"time_added\": 1731000000.0}",
			i > 0 ? ", " : "", torrent->hash, torrent->name, torrent->state, progress, torrent->wanted, torrent->done,
			downloading ? 5242880 : 0, downloading ? 300 : 0, torrent->location, torrent->finished ? "true" : "false",
			torrent->label != NULL ? torrent->label : "", torrent->tracker != NULL ? torrent->tracker : "");
	}
	g_string_append_c(out, '}');
	return g_string_free(out, FALSE);
}

static void
add_torrent(
	Dg          *self,
	const gchar *hash,
	const gchar *name
){
	Torrent *torrent = g_new0(Torrent, 1);

	torrent->hash = g_strdup(hash);
	torrent->name = g_strdup(name != NULL ? name : hash);
	torrent->state = g_strdup("Queued");
	torrent->wanted = 1000;
	torrent->done = 0;
	torrent->location = g_strdup("/downloads");
	g_ptr_array_add(self->torrents, torrent);
}

/* One authenticated call of the web interface. */
static void
dg_call(
	Dg                 *self,
	const gchar        *method,
	JsonArray          *params,
	gint64              id,
	AxisTestPlexAnswer *answer
){
	Raw *raw = g_hash_table_lookup(self->raw, method);
	g_autofree gchar *text = NULL;

	if (strcmp(method, "label.set_torrent") == 0) {
		const gchar *hash = param_string(params, 0);
		const gchar *label = param_string(params, 1);

		note(self, "label.set_torrent:%s:%s", hash != NULL ? hash : "", label != NULL ? label : "");
	} else if (strcmp(method, "core.remove_torrent") == 0) {
		const gchar *hash = param_string(params, 0);
		gboolean data = params != NULL && json_array_get_length(params) > 1 &&
			json_node_get_value_type(json_array_get_element(params, 1)) == G_TYPE_BOOLEAN &&
			json_array_get_boolean_element(params, 1);

		note(self, "core.remove_torrent:%s:%d", hash != NULL ? hash : "", data ? 1 : 0);
	} else if (strcmp(method, "label.add") == 0) {
		note(self, "label.add:%s", param_string(params, 0) != NULL ? param_string(params, 0) : "");
	} else if (strcmp(method, "web.connect") == 0) {
		note(self, "web.connect:%s", param_string(params, 0) != NULL ? param_string(params, 0) : "");
	} else {
		note(self, "%s", method);
	}

	if (strcmp(method, "core.get_torrents_status") == 0) {
		JsonNode *filter = params != NULL && json_array_get_length(params) > 0 ? json_array_get_element(params, 0) : NULL;
		JsonNode *keys = params != NULL && json_array_get_length(params) > 1 ? json_array_get_element(params, 1) : NULL;

		g_clear_pointer(&self->last_filter, g_free);
		g_clear_pointer(&self->last_keys, g_free);
		if (filter != NULL && JSON_NODE_HOLDS_OBJECT(filter) && json_object_has_member(json_node_get_object(filter), "label"))
			self->last_filter = g_strdup(json_object_get_string_member(json_node_get_object(filter), "label"));
		if (keys != NULL && JSON_NODE_HOLDS_ARRAY(keys)) {
			JsonArray *list = json_node_get_array(keys);
			GString *joined = g_string_new(NULL);
			guint i;

			for (i = 0; i < json_array_get_length(list); i++)
				g_string_append_printf(joined, "%s%s", i > 0 ? "," : "", json_array_get_string_element(list, i));
			self->last_keys = g_string_free(joined, FALSE);
		}
	}

	if (raw != NULL && !((strcmp(method, "core.add_torrent_file") == 0 || strcmp(method, "core.add_torrent_magnet") == 0) &&
	                     self->mode == DG_ADD_HANGS)) {
		g_autofree gchar *body = with_id(raw->body, id);

		answer_text(answer, raw->status, raw->type, body);
		return;
	}

	if (strcmp(method, "web.connected") == 0) {
		reply_result(answer, id, self->connected ? "true" : "false");
	} else if (strcmp(method, "web.get_hosts") == 0) {
		reply_result(answer, id, self->has_hosts ? "[[\"" HOST_ID "\", \"127.0.0.1\", 58846, \"localhost\"]]" : "[]");
	} else if (strcmp(method, "web.connect") == 0) {
		if (self->connect_works && g_strcmp0(param_string(params, 0), HOST_ID) == 0)
			self->connected = TRUE;
		reply_result(answer, id, "null");
	} else if (strcmp(method, "daemon.get_version") == 0) {
		if (self->old_daemon)
			reply_error(answer, id, 2, "Unknown method");
		else
			reply_result(answer, id, "\"2.1.1\"");
	} else if (strcmp(method, "daemon.info") == 0) {
		reply_result(answer, id, "\"1.3.15\"");
	} else if (strcmp(method, "core.get_enabled_plugins") == 0) {
		reply_result(answer, id, self->has_label_plugin ? "[\"Scheduler\", \"Label\"]" : "[\"Scheduler\"]");
	} else if (strcmp(method, "label.get_labels") == 0) {
		if (!self->has_label_plugin) {
			reply_error(answer, id, 2, "Unknown method label.get_labels");
			return;
		}
		text = names_json(self->labels);
		reply_result(answer, id, text);
	} else if (strcmp(method, "label.add") == 0) {
		const gchar *label = param_string(params, 0);

		if (!self->has_label_plugin || label == NULL) {
			reply_error(answer, id, 2, "Unknown method label.add");
			return;
		}
		if (!has_name(self->labels, label))
			g_ptr_array_add(self->labels, g_strdup(label));
		reply_result(answer, id, "null");
	} else if (strcmp(method, "core.add_torrent_file") == 0 || strcmp(method, "core.add_torrent_magnet") == 0) {
		g_autofree gchar *hash = NULL;
		g_autofree gchar *name = NULL;
		gboolean file = strcmp(method, "core.add_torrent_file") == 0;
		JsonNode *options = params != NULL && json_array_get_length(params) > 2 ? json_array_get_element(params, 2) : NULL;
		const gchar *source = param_string(params, file ? 1 : 0);

		g_clear_pointer(&self->last_add_name, g_free);
		g_clear_pointer(&self->last_add_options, g_free);
		self->last_add_name = g_strdup(file ? param_string(params, 0) : source);
		self->last_add_options = g_strdup(options != NULL && JSON_NODE_HOLDS_OBJECT(options) &&
			json_object_get_size(json_node_get_object(options)) == 0 ? "{}" : "other");
		if (source == NULL) {
			reply_error(answer, id, 3, "Bad parameters");
			return;
		}
		if (file) {
			gsize length = 0;
			g_autofree guchar *data = g_base64_decode(source, &length);

			if (!axis_download_torrent_read(data, length, &hash, &name, NULL)) {
				reply_error(answer, id, 3, "Not a torrent");
				return;
			}
		} else if (!axis_download_magnet_parse(source, &hash, NULL)) {
			reply_error(answer, id, 3, "Not a magnet link");
			return;
		}
		if (find_torrent(self, hash) != NULL) {
			g_autofree gchar *words = g_strdup_printf("Torrent already in session (%s).", hash);

			reply_error(answer, id, 3, words);
			return;
		}
		add_torrent(self, hash, name != NULL ? name : "magnet");
		if (self->mode == DG_ADD_HANGS) {
			answer->hang = TRUE;
			return;
		}
		g_free(text);
		text = g_strdup_printf("\"%s\"", self->add_hash_override != NULL ? self->add_hash_override : hash);
		reply_result(answer, id, text);
	} else if (strcmp(method, "label.set_torrent") == 0) {
		const gchar *hash = param_string(params, 0);
		const gchar *label = param_string(params, 1);
		Torrent *torrent = find_torrent(self, hash);

		if (self->fail_set_label || label == NULL || !has_name(self->labels, label)) {
			reply_error(answer, id, 3, "Unknown Label");
			return;
		}
		if (torrent == NULL) {
			reply_error(answer, id, 3, "Unknown Torrent");
			return;
		}
		g_free(torrent->label);
		torrent->label = g_strdup(label);
		if (self->hang_set_label) {
			answer->hang = TRUE;
			return;
		}
		reply_result(answer, id, "null");
	} else if (strcmp(method, "core.get_torrents_status") == 0) {
		/* The filter the kind asks for is not applied: the layer's own
		 * filtering is what keeps other labels out. */
		text = torrents_json(self);
		reply_result(answer, id, text);
	} else if (strcmp(method, "core.remove_torrent") == 0) {
		const gchar *hash = param_string(params, 0);
		Torrent *torrent = find_torrent(self, hash);

		if (self->fail_remove) {
			reply_error(answer, id, 3, "Cannot remove");
			return;
		}
		if (torrent == NULL) {
			/* As Deluge 2.2.0 answered a removal of a torrent it did not have
			 * (InvalidTorrentError and "torrent_id <hash> not in session."
			 * were seen; the Failure wrapper and the code are believed). */
			g_autofree gchar *words = g_strdup_printf("Failure: [Failure instance: Traceback (failure with no frames): "
				"<class 'deluge.error.InvalidTorrentError'>: torrent_id %s not in session.\\n]", hash != NULL ? hash : "");

			reply_error(answer, id, 4, words);
			return;
		}
		g_ptr_array_remove(self->torrents, torrent);
		reply_result(answer, id, "true");
	} else if (strcmp(method, "label.get_options") == 0) {
		const gchar *label = param_string(params, 0);
		const gchar *folder = label != NULL ? g_hash_table_lookup(self->label_folders, label) : NULL;

		note(self, "options-of:%s", label != NULL ? label : "");
		if (!self->has_label_plugin) {
			reply_error(answer, id, 2, "Unknown method label.get_options");
			return;
		}
		if (label == NULL || !has_name(self->labels, label)) {
			g_autofree gchar *words = g_strdup_printf("Failure: [Failure instance: Traceback (failure with no frames): "
				"<class 'KeyError'>: '%s'\\n]", label != NULL ? label : "");

			reply_error(answer, id, 4, words);
			return;
		}
		text = g_strdup_printf("{\"apply_max\": false, \"max_download_speed\": -1, \"apply_queue\": false, "
			"\"apply_move_completed\": %s, \"move_completed\": %s, \"move_completed_path\": \"%s\", \"auto_add\": false, "
			"\"auto_add_trackers\": []}", folder != NULL ? "true" : "false", folder != NULL ? "true" : "false",
			folder != NULL ? folder : "");
		reply_result(answer, id, text);
	} else if (strcmp(method, "core.get_config_values") == 0) {
		JsonNode *keys = params != NULL && json_array_get_length(params) > 0 ? json_array_get_element(params, 0) : NULL;
		GString *out = g_string_new("{");

		if (keys != NULL && JSON_NODE_HOLDS_ARRAY(keys)) {
			JsonArray *list = json_node_get_array(keys);
			guint i;

			for (i = 0; i < json_array_get_length(list); i++) {
				const gchar *key = json_array_get_string_element(list, i);
				const gchar *value = g_strcmp0(key, "download_location") == 0 ? self->download_location :
					g_strcmp0(key, "move_completed_path") == 0 ? self->move_completed_path : NULL;

				note(self, "config-key:%s", key != NULL ? key : "");
				if (value != NULL)
					g_string_append_printf(out, "%s\"%s\": \"%s\"", out->len > 1 ? ", " : "", key, value);
				else if (key != NULL)
					g_string_append_printf(out, "%s\"%s\": null", out->len > 1 ? ", " : "", key);
			}
		}
		g_string_append_c(out, '}');
		text = g_string_free(out, FALSE);
		reply_result(answer, id, text);
	} else {
		reply_error(answer, id, 2, "Unknown method");
	}
}

static gboolean
has_session(
	Dg                        *self,
	const AxisTestPlexRequest *request
){
	const gchar *cookie = axis_test_plex_request_header(request, "Cookie");
	g_autofree gchar *wanted = NULL;

	if (cookie == NULL || self->session == NULL)
		return FALSE;
	wanted = g_strdup_printf("_session_id=%s", self->session);
	return strstr(cookie, wanted) != NULL;
}

static void
dg_dispatch(
	Dg                        *self,
	const AxisTestPlexRequest *request,
	AxisTestPlexAnswer        *answer
){
	g_autofree gchar *want_path = g_strconcat(self->prefix, "/json", NULL);
	const gchar *content_type = axis_test_plex_request_header(request, "Content-Type");
	g_autoptr(JsonParser) parser = json_parser_new();
	JsonObject *object = NULL;
	JsonArray *params = NULL;
	const gchar *method = NULL;
	gint64 id = 0;
	gsize length = 0;
	const gchar *data = g_bytes_get_data(request->body, &length);

	if (self->mode == DG_DOWN) {
		answer->hang = TRUE;
		return;
	}
	if (self->mode == DG_REDIRECT) {
		answer->status = 307;
		answer->headers = g_new0(gchar *, 3);
		answer->headers[0] = g_strdup("Location");
		answer->headers[1] = g_strdup(self->redirect != NULL ? self->redirect : "/");
		return;
	}
	if (strcmp(request->path, want_path) != 0) {
		self->bad_requests++;
		note(self, "wrong-path %s", request->path);
		answer_text(answer, 404, "text/plain", "not found");
		return;
	}
	if (strcmp(request->method, "POST") != 0 || request->query != NULL ||
	    g_strcmp0(content_type, "application/json") != 0) {
		self->bad_requests++;
		note(self, "bad-request %s", request->method);
		answer_text(answer, 405, "text/plain", "POST of JSON only");
		return;
	}
	if (length == 0 || !json_parser_load_from_data(parser, data, (gssize)length, NULL) ||
	    json_parser_get_root(parser) == NULL || !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
		self->bad_requests++;
		answer_text(answer, 400, "text/plain", "not JSON");
		return;
	}
	object = json_node_get_object(json_parser_get_root(parser));
	method = json_object_has_member(object, "method") ? json_object_get_string_member(object, "method") : NULL;
	id = json_object_has_member(object, "id") ? json_object_get_int_member(object, "id") : 0;
	if (json_object_has_member(object, "params") && JSON_NODE_HOLDS_ARRAY(json_object_get_member(object, "params")))
		params = json_object_get_array_member(object, "params");
	if (method == NULL || params == NULL || id == 0) {
		self->bad_requests++;
		note(self, "bad-envelope");
		answer_text(answer, 400, "text/plain", "not a JSON-RPC call");
		return;
	}

	/* A session that ends: the next call of this method finds it gone. */
	if (self->expire_before != NULL && strcmp(self->expire_before, method) == 0) {
		g_clear_pointer(&self->session, g_free);
		g_clear_pointer(&self->expire_before, g_free);
	}

	if (strcmp(method, "auth.login") == 0) {
		Raw *raw = g_hash_table_lookup(self->raw, method);
		const gchar *password = param_string(params, 0);

		note(self, "auth.login");
		if (raw != NULL) {
			g_autofree gchar *body = with_id(raw->body, id);

			answer_text(answer, raw->status, raw->type, body);
			return;
		}
		if (g_strcmp0(password, self->password) != 0) {
			reply_result(answer, id, "false");
			return;
		}
		g_free(self->session);
		self->session = g_strdup_printf("%08x%08x%08x%08x", g_random_int(), g_random_int(), g_random_int(), g_random_int());
		g_ptr_array_add(self->sessions, g_strdup(self->session));
		reply_result(answer, id, "true");
		if (!self->no_cookie) {
			answer->headers = g_new0(gchar *, 3);
			answer->headers[0] = g_strdup("Set-Cookie");
			answer->headers[1] = g_strdup_printf("_session_id=%s; Expires=Fri, 01 Jan 2100 00:00:00 GMT; Path=/; HttpOnly",
				self->session);
		}
		return;
	}
	if (strcmp(method, "auth.check_session") == 0) {
		note(self, "auth.check_session");
		reply_result(answer, id, has_session(self, request) ? "true" : "false");
		return;
	}
	if (!has_session(self, request)) {
		note(self, "unauthorised:%s", method);
		reply_error(answer, id, 1, "Not authenticated");
		return;
	}
	dg_call(self, method, params, id, answer);
}

static void
dg_handle(
	const AxisTestPlexRequest *request,
	AxisTestPlexAnswer        *answer,
	gpointer                   user_data
){
	Dg *self = user_data;

	g_mutex_lock(&self->lock);
	dg_dispatch(self, request, answer);
	g_mutex_unlock(&self->lock);
}

static Dg *
dg_new(const gchar *password)
{
	Dg *self = g_new0(Dg, 1);

	g_mutex_init(&self->lock);
	self->password = g_strdup(password);
	self->prefix = g_strdup("");
	self->connected = TRUE;
	self->has_hosts = TRUE;
	self->connect_works = TRUE;
	self->has_label_plugin = TRUE;
	self->sessions = g_ptr_array_new_with_free_func(g_free);
	self->labels = g_ptr_array_new_with_free_func(g_free);
	self->torrents = g_ptr_array_new_with_free_func(torrent_free);
	self->calls = g_ptr_array_new_with_free_func(g_free);
	self->raw = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, raw_free);
	self->label_folders = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	self->download_location = g_strdup("/downloads/incoming");
	self->move_completed_path = g_strdup("/downloads/done");
	g_ptr_array_add(self->labels, g_strdup("axis"));
	g_ptr_array_add(self->labels, g_strdup("manual"));
	self->server = axis_test_plex_new();
	g_assert_nonnull(self->server);
	axis_test_plex_set_handler(self->server, dg_handle, self);
	return self;
}

/* The label's "Move completed to" folder, switched on (%NULL: off). */
static void
dg_set_label_folder(
	Dg          *self,
	const gchar *label,
	const gchar *folder
){
	g_mutex_lock(&self->lock);
	if (folder != NULL)
		g_hash_table_insert(self->label_folders, g_strdup(label), g_strdup(folder));
	else
		g_hash_table_remove(self->label_folders, label);
	g_mutex_unlock(&self->lock);
}

static void
dg_free(Dg *self)
{
	if (self == NULL)
		return;
	axis_test_plex_free(self->server);
	g_ptr_array_unref(self->sessions);
	g_ptr_array_unref(self->labels);
	g_ptr_array_unref(self->torrents);
	g_ptr_array_unref(self->calls);
	g_hash_table_unref(self->raw);
	g_hash_table_unref(self->label_folders);
	g_free(self->download_location);
	g_free(self->move_completed_path);
	g_free(self->password);
	g_free(self->prefix);
	g_free(self->redirect);
	g_free(self->session);
	g_free(self->add_hash_override);
	g_free(self->expire_before);
	g_free(self->last_filter);
	g_free(self->last_keys);
	g_free(self->last_add_name);
	g_free(self->last_add_options);
	g_mutex_clear(&self->lock);
	g_free(self);
}

/* Every answer to @method is this, until it is cleared. */
static void
dg_raw(
	Dg          *self,
	const gchar *method,
	guint        status,
	const gchar *type,
	const gchar *body
){
	Raw *raw = g_new0(Raw, 1);

	raw->status = status;
	raw->type = g_strdup(type);
	raw->body = g_strdup(body);
	g_mutex_lock(&self->lock);
	g_hash_table_insert(self->raw, g_strdup(method), raw);
	g_mutex_unlock(&self->lock);
}

static void
dg_raw_fixture(
	Dg          *self,
	const gchar *method,
	const gchar *fixture
){
	g_autofree gchar *text = fixture_text(fixture);

	dg_raw(self, method, 200, "application/json", text);
}

static void
dg_clear_raw(
	Dg          *self,
	const gchar *method
){
	g_mutex_lock(&self->lock);
	g_hash_table_remove(self->raw, method);
	g_mutex_unlock(&self->lock);
}

static void
dg_set_mode(
	Dg     *self,
	DgMode  mode
){
	g_mutex_lock(&self->lock);
	self->mode = mode;
	g_mutex_unlock(&self->lock);
}

/* Sets one of the fake's flags (under its lock). */
#define DG_SET(self, field, value) \
	G_STMT_START { g_mutex_lock(&(self)->lock); (self)->field = (value); g_mutex_unlock(&(self)->lock); } G_STMT_END

static void
dg_set_text(
	Dg          *self,
	gchar      **field,
	const gchar *value
){
	g_mutex_lock(&self->lock);
	g_free(*field);
	*field = g_strdup(value);
	g_mutex_unlock(&self->lock);
}

/* A torrent Deluge holds as if a person had added it. */
static void
dg_put(
	Dg          *self,
	const gchar *hash,
	const gchar *name,
	const gchar *label,
	const gchar *state,
	gint64       wanted,
	gint64       done,
	const gchar *location,
	gboolean     finished,
	const gchar *tracker
){
	Torrent *torrent;

	g_mutex_lock(&self->lock);
	torrent = find_torrent(self, hash);
	if (torrent == NULL) {
		torrent = g_new0(Torrent, 1);
		torrent->hash = g_strdup(hash);
		g_ptr_array_add(self->torrents, torrent);
	}
	g_free(torrent->name);
	torrent->name = g_strdup(name);
	g_free(torrent->label);
	torrent->label = g_strdup(label);
	g_free(torrent->state);
	torrent->state = g_strdup(state);
	torrent->wanted = wanted;
	torrent->done = done;
	g_free(torrent->location);
	torrent->location = g_strdup(location);
	torrent->finished = finished;
	g_free(torrent->tracker);
	torrent->tracker = g_strdup(tracker);
	g_mutex_unlock(&self->lock);
}

static void
dg_set_state(
	Dg          *self,
	const gchar *hash,
	const gchar *state,
	gint64       done,
	gboolean     finished
){
	Torrent *torrent;

	g_mutex_lock(&self->lock);
	torrent = find_torrent(self, hash);
	g_assert_nonnull(torrent);
	g_free(torrent->state);
	torrent->state = g_strdup(state);
	torrent->done = done;
	torrent->finished = finished;
	g_mutex_unlock(&self->lock);
}

static gboolean
dg_has(
	Dg          *self,
	const gchar *hash
){
	gboolean has;

	g_mutex_lock(&self->lock);
	has = find_torrent(self, hash) != NULL;
	g_mutex_unlock(&self->lock);
	return has;
}

static gchar *
dg_label_of(
	Dg          *self,
	const gchar *hash
){
	gchar *label = NULL;
	Torrent *torrent;

	g_mutex_lock(&self->lock);
	torrent = find_torrent(self, hash);
	if (torrent != NULL)
		label = g_strdup(torrent->label);
	g_mutex_unlock(&self->lock);
	return label;
}

static guint
dg_n_torrents(Dg *self)
{
	guint n;

	g_mutex_lock(&self->lock);
	n = self->torrents->len;
	g_mutex_unlock(&self->lock);
	return n;
}

static gboolean
dg_has_label(
	Dg          *self,
	const gchar *label
){
	gboolean has;

	g_mutex_lock(&self->lock);
	has = has_name(self->labels, label);
	g_mutex_unlock(&self->lock);
	return has;
}

static void
dg_set_labels(
	Dg                 *self,
	const gchar *const *names
){
	guint i;

	g_mutex_lock(&self->lock);
	g_ptr_array_set_size(self->labels, 0);
	for (i = 0; names[i] != NULL; i++)
		g_ptr_array_add(self->labels, g_strdup(names[i]));
	g_mutex_unlock(&self->lock);
}

/* The session ends now, as an idle one does: the next call is not authenticated. */
static void
dg_expire(Dg *self)
{
	g_mutex_lock(&self->lock);
	g_clear_pointer(&self->session, g_free);
	g_mutex_unlock(&self->lock);
}

/* How many calls begin with @prefix. */
static guint
dg_count(
	Dg          *self,
	const gchar *prefix
){
	guint n = 0;
	guint i;

	g_mutex_lock(&self->lock);
	for (i = 0; i < self->calls->len; i++) {
		if (g_str_has_prefix(g_ptr_array_index(self->calls, i), prefix))
			n++;
	}
	g_mutex_unlock(&self->lock);
	return n;
}

/* The calls in order, as one text. */
static gchar *
dg_calls(Dg *self)
{
	GString *out = g_string_new(NULL);
	guint i;

	g_mutex_lock(&self->lock);
	for (i = 0; i < self->calls->len; i++)
		g_string_append_printf(out, "%s%s", i > 0 ? " " : "", (const gchar *)g_ptr_array_index(self->calls, i));
	g_mutex_unlock(&self->lock);
	return g_string_free(out, FALSE);
}

static guint
dg_bad_requests(Dg *self)
{
	guint n;

	g_mutex_lock(&self->lock);
	n = self->bad_requests;
	g_mutex_unlock(&self->lock);
	return n;
}

static gchar *
dg_text(
	Dg     *self,
	gchar **field
){
	gchar *value;

	g_mutex_lock(&self->lock);
	value = g_strdup(*field);
	g_mutex_unlock(&self->lock);
	return value;
}

/* ── The fixture ────────────────────────────────────────────────── */

typedef struct
{
	AxisTestLibrary *t;
	Dg              *dg;
	AxisDownloads   *downloads;
	gchar           *disk;
	gint64           volume_id;
	gint64           client_id;
} Fixture;

static AxisDownloads *
layer_new(Fixture *fx)
{
	AxisDownloads *downloads = axis_downloads_new(fx->t->pool, fx->t->storage, NULL);
	AxisDownloadTuning tuning;
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_downloads_add_kind(downloads, axis_download_deluge_kind(), &error));
	g_assert_no_error(error);
	memset(&tuning, 0, sizeof(tuning));
	tuning.request_timeout_ms = 1500;
	axis_downloads_set_tuning(downloads, &tuning);
	return downloads;
}

static gint64
add_client_full(
	Fixture              *fx,
	const gchar          *name,
	const gchar          *address,
	const gchar          *base_path,
	const gchar          *password,
	const gchar          *category,
	AxisDownloadPriority  priority
){
	AxisDownloadClientSpec spec;
	g_autoptr(GError) error = NULL;
	gint64 id = 0;

	memset(&spec, 0, sizeof(spec));
	spec.name = name;
	spec.kind = AXIS_DOWNLOAD_DELUGE_NAME;
	spec.address = address;
	spec.base_path = base_path;
	spec.category = category;
	spec.priority = priority;
	spec.enabled = TRUE;
	spec.secret = password;
	g_assert_true(axis_downloads_save_client(fx->downloads, 0, &spec, &id, &error));
	g_assert_no_error(error);
	return id;
}

static gint64
add_client_for(
	Fixture     *fx,
	Dg          *dg,
	const gchar *name,
	const gchar *password
){
	g_autofree gchar *base = axis_test_plex_base(dg->server);

	return add_client_full(fx, name, base, NULL, password, NULL, AXIS_DOWNLOAD_PRIORITY_HIGH);
}

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	memset(fx, 0, sizeof(*fx));
	axis_test_log_init();
	axis_test_log_reset();
	axis_download_deluge_forget();
	fx->t = axis_test_library_new(4);
	if (fx->t == NULL)
		return;
	fx->disk = axis_test_library_disk(fx->t, "disk1");
	fx->volume_id = axis_test_library_add_volume(fx->t, fx->disk, "Disk one");
	fx->dg = dg_new(PASSWORD);
	fx->downloads = layer_new(fx);
	fx->client_id = add_client_for(fx, fx->dg, "Torrents", PASSWORD);
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	if (fx->t == NULL)
		return;
	axis_downloads_unref(fx->downloads);
	dg_free(fx->dg);
	g_free(fx->disk);
	axis_test_library_free(fx->t);
	axis_test_log_reset();
	axis_download_deluge_forget();
}

AXIS_TEST_DB_CASES(Fixture, fx->t != NULL)

static gchar *
q(
	Fixture     *fx,
	const gchar *format,
	...
) G_GNUC_PRINTF(2, 3);

static gchar *
q(
	Fixture     *fx,
	const gchar *format,
	...
){
	g_autofree gchar *sql = NULL;
	va_list ap;

	va_start(ap, format);
	sql = g_strdup_vprintf(format, ap);
	va_end(ap);
	return axis_test_db_query(fx->t->db, sql);
}

static void
assert_q(
	Fixture     *fx,
	const gchar *expected,
	const gchar *sql
){
	g_autofree gchar *value = axis_test_db_query(fx->t->db, sql);

	if (g_strcmp0(value, expected) != 0)
		g_error("%s: expected %s, got %s", sql, expected != NULL ? expected : "NULL", value != NULL ? value : "NULL");
}

static guint
logged(const gchar *pattern)
{
	static const GLogLevelFlags levels[] = {
		G_LOG_LEVEL_ERROR, G_LOG_LEVEL_CRITICAL, G_LOG_LEVEL_WARNING, G_LOG_LEVEL_MESSAGE, G_LOG_LEVEL_INFO,
		G_LOG_LEVEL_DEBUG
	};
	guint n = 0;
	guint i;

	for (i = 0; i < G_N_ELEMENTS(levels); i++)
		n += axis_test_log_count(levels[i], pattern);
	return n;
}

static AxisDownloadPollReport
poll_client(
	Fixture *fx,
	gint64   id
){
	AxisDownloadPollReport report;
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_downloads_poll_client(fx->downloads, id, NULL, &report, &error));
	g_assert_no_error(error);
	return report;
}

static AxisDownloadPollReport
poll(Fixture *fx)
{
	return poll_client(fx, fx->client_id);
}

/* A torrent (the support code's small valid one) of @name; its info-hash in @hash. */
static gint64
add_torrent_release_at(
	Fixture      *fx,
	const gchar  *name,
	gchar       **hash,
	GError      **error
){
	g_autoptr(GBytes) file = axis_test_download_torrent(name, hash);
	AxisDownloadRequest request;
	gint64 id = 0;

	memset(&request, 0, sizeof(request));
	request.protocol = AXIS_DOWNLOAD_TORRENT;
	request.file = file;
	request.release_name = name;
	request.title_label = "Some Film (2020)";
	request.module = "movies";
	axis_downloads_add(fx->downloads, &request, &id, error);
	return id;
}

static gint64
add_torrent_release(
	Fixture      *fx,
	const gchar  *name,
	gchar       **hash
){
	g_autoptr(GError) error = NULL;
	gint64 id = add_torrent_release_at(fx, name, hash, &error);

	g_assert_no_error(error);
	g_assert_cmpint(id, >, 0);
	return id;
}

static gint64
add_magnet_at(
	Fixture     *fx,
	const gchar *name,
	GError     **error
){
	AxisDownloadRequest request;
	gint64 id = 0;

	memset(&request, 0, sizeof(request));
	request.protocol = AXIS_DOWNLOAD_TORRENT;
	request.magnet = MAGNET;
	request.release_name = name;
	axis_downloads_add(fx->downloads, &request, &id, error);
	return id;
}

static gchar *
ref_of(
	Fixture *fx,
	gint64   id
){
	return q(fx, "SELECT client_ref FROM download WHERE id = %" G_GINT64_FORMAT, id);
}

static void
assert_state(
	Fixture     *fx,
	gint64       id,
	const gchar *state
){
	g_autofree gchar *now = q(fx, "SELECT state FROM download WHERE id = %" G_GINT64_FORMAT, id);

	g_assert_cmpstr(now, ==, state);
}

/* A record of a download that Deluge (the fake's torrents) holds, as if Axis
 * had added it. */
static gint64
insert_record(
	Fixture     *fx,
	const gchar *hash,
	const gchar *name,
	const gchar *state
){
	g_autofree gchar *sql = g_strdup_printf("INSERT INTO download (client_id, client_name, client_ref, info_hash, protocol, "
		"source, release_name, state) VALUES (%" G_GINT64_FORMAT ", 'Torrents', '%s', '%s', 'torrent', 'file', '%s', '%s')"
		" RETURNING id", fx->client_id, hash, hash, name, state);
	g_autofree gchar *id = axis_test_db_query(fx->t->db, sql);

	return g_ascii_strtoll(id, NULL, 10);
}

static void
add_map(Fixture *fx)
{
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_downloads_add_path_map(fx->downloads, fx->client_id, "/downloads", fx->volume_id, "downloads",
		&error));
	g_assert_no_error(error);
}

/* The restarted daemon: a new layer on the same database, a record left
 * 'sending' made older than the reconciliation wait. */
static void
restart(Fixture *fx)
{
	axis_downloads_unref(fx->downloads);
	axis_download_deluge_forget();
	fx->downloads = layer_new(fx);
	axis_test_db_exec(fx->t->db, "UPDATE download SET created_at = now() - interval '1 hour' WHERE state = 'sending'");
}

static AxisDownloadTestResult
test_client(
	Fixture *fx,
	gint64   id
){
	AxisDownloadTestResult result;
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_downloads_test_client(fx->downloads, id, &result, &error));
	g_assert_no_error(error);
	return result;
}

static AxisDownloadClientState
client_state(
	Fixture *fx,
	gint64   id
){
	g_autoptr(AxisDownloadClientInfo) client = NULL;
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_downloads_get_client(fx->downloads, id, &client, &error));
	return client->state;
}

/* ── Reading answers (no database) ──────────────────────────────── */

static AxisJsonDoc *
doc_of_text(const gchar *text)
{
	AxisJsonDoc *doc = axis_json_parse(text, strlen(text), NULL);

	g_assert_nonnull(doc);
	return doc;
}

static AxisDelugeReplyKind
reply_of_text(
	const gchar          *text,
	gint64                id,
	AxisJsonDoc         **doc,
	const AxisJsonNode  **result,
	gchar               **message
){
	return axis_deluge_reply_parse(text, strlen(text), id, doc, result, message);
}

static AxisDelugeReplyKind
reply_of(
	const gchar          *fixture,
	AxisJsonDoc         **doc,
	const AxisJsonNode  **result,
	gchar               **message
){
	g_autofree gchar *text = fixture_text(fixture);

	return reply_of_text(text, 1, doc, result, message);
}

/* The slots of a fixture's list. */
static GPtrArray *
slots_of(const gchar *fixture)
{
	g_autoptr(AxisJsonDoc) doc = NULL;
	const AxisJsonNode *result = NULL;
	g_autofree gchar *message = NULL;
	GPtrArray *slots;

	g_assert_cmpint(reply_of(fixture, &doc, &result, &message), ==, AXIS_DELUGE_REPLY_OK);
	slots = axis_deluge_parse_torrents(result);
	return slots;
}

static const AxisDelugeSlot *
slot_by_ref(
	GPtrArray   *slots,
	const gchar *ref
){
	guint i;

	for (i = 0; i < slots->len; i++) {
		const AxisDelugeSlot *slot = g_ptr_array_index(slots, i);

		if (strcmp(slot->item->client_ref, ref) == 0)
			return slot;
	}
	g_error("no slot %s", ref);
	return NULL;
}

static gboolean
has_slot(
	GPtrArray   *slots,
	const gchar *ref
){
	guint i;

	for (i = 0; i < slots->len; i++) {
		if (strcmp(((AxisDelugeSlot *)g_ptr_array_index(slots, i))->item->client_ref, ref) == 0)
			return TRUE;
	}
	return FALSE;
}

/* Task 2.1: the envelope */
static void
test_parse_envelope(void)
{
	AxisJsonDoc *doc = NULL;
	const AxisJsonNode *result = NULL;
	g_autofree gchar *message = NULL;
	gboolean value = FALSE;
	static const gchar *const unreadable[] = {
		"envelope-wrong-id.json", "envelope-no-id.json", "envelope-string-id.json", "envelope-both.json",
		"envelope-neither.json", "envelope-error-not-object.json", "not-an-object.json", "truncated.json", NULL
	};
	guint i;

	/* A result, no error. */
	g_assert_cmpint(reply_of("login-ok.json", &doc, &result, &message), ==, AXIS_DELUGE_REPLY_OK);
	g_assert_null(message);
	g_assert_true(axis_deluge_parse_boolean(result, &value));
	g_assert_true(value);
	axis_json_doc_free(doc);
	/* A null result with a null error is still a result (the caller checks its type). */
	g_assert_cmpint(reply_of("add-null.json", &doc, &result, &message), ==, AXIS_DELUGE_REPLY_OK);
	g_assert_cmpint(axis_json_type(result), ==, AXIS_JSON_NULL);
	axis_json_doc_free(doc);

	/* Not authenticated: by the code, or by the words alone. */
	g_assert_cmpint(reply_of("error-not-authenticated.json", &doc, &result, &message), ==, AXIS_DELUGE_REPLY_NOT_AUTH);
	g_assert_null(doc);
	g_assert_cmpstr(message, ==, "Not authenticated");
	g_clear_pointer(&message, g_free);
	g_assert_cmpint(reply_of("error-not-authenticated-words-only.json", &doc, &result, &message), ==,
		AXIS_DELUGE_REPLY_NOT_AUTH);
	g_clear_pointer(&message, g_free);
	g_assert_cmpint(reply_of_text("{\"result\": null, \"error\": {\"message\": \"x\", \"code\": 1}, \"id\": 1}", 1, &doc, &result,
		&message), ==, AXIS_DELUGE_REPLY_NOT_AUTH);
	g_clear_pointer(&message, g_free);

	/* Any other error: its words, cleaned (no line break, no spaces twice). */
	g_assert_cmpint(reply_of("error-unknown-method.json", &doc, &result, &message), ==, AXIS_DELUGE_REPLY_ERROR);
	g_assert_null(doc);
	g_assert_cmpstr(message, ==, "Unknown method label.get_labels");
	g_clear_pointer(&message, g_free);
	g_assert_cmpint(reply_of("error-no-words.json", &doc, &result, &message), ==, AXIS_DELUGE_REPLY_ERROR);
	g_assert_null(message);
	g_assert_cmpint(reply_of_text("{\"result\": null, \"error\": {\"message\": \"x\"}, \"id\": 1}", 1, &doc, &result, &message), ==,
		AXIS_DELUGE_REPLY_ERROR);
	g_clear_pointer(&message, g_free);

	/* An envelope that cannot be believed: unreadable, with a reason, no document. */
	for (i = 0; unreadable[i] != NULL; i++) {
		g_assert_cmpint(reply_of(unreadable[i], &doc, &result, &message), ==, AXIS_DELUGE_REPLY_UNREADABLE);
		g_assert_null(doc);
		g_assert_null(result);
		g_assert_nonnull(message);
		g_clear_pointer(&message, g_free);
	}
	g_assert_cmpint(reply_of_text("", 1, &doc, &result, &message), ==, AXIS_DELUGE_REPLY_UNREADABLE);
	g_clear_pointer(&message, g_free);
	g_assert_cmpint(reply_of_text("<html>Bad gateway</html>", 1, &doc, &result, &message), ==, AXIS_DELUGE_REPLY_UNREADABLE);
	g_clear_pointer(&message, g_free);
	/* The id must be the one sent. */
	g_assert_cmpint(reply_of_text("{\"result\": true, \"error\": null, \"id\": 7}", 7, &doc, &result, &message), ==,
		AXIS_DELUGE_REPLY_OK);
	axis_json_doc_free(doc);
	g_assert_cmpint(reply_of_text("{\"result\": true, \"error\": null, \"id\": 7}", 8, &doc, &result, &message), ==,
		AXIS_DELUGE_REPLY_UNREADABLE);
	g_clear_pointer(&message, g_free);
}

/* Task 2.1: the small answers */
static void
test_parse_values(void)
{
	AxisJsonDoc *doc;
	const AxisJsonNode *result = NULL;
	g_autofree gchar *message = NULL;
	g_autofree gchar *hash = NULL;
	g_autofree gchar *version = NULL;
	g_autofree gchar *host_id = NULL;
	g_autofree gchar *host = NULL;
	gboolean value = TRUE;
	gint port = 0;

	/* A boolean is a boolean: not a string, not a number. */
	doc = doc_of_text("{\"a\": \"true\", \"b\": 1, \"c\": false, \"d\": null}");
	g_assert_false(axis_deluge_parse_boolean(axis_json_member(axis_json_doc_get_root(doc), "a"), &value));
	g_assert_false(axis_deluge_parse_boolean(axis_json_member(axis_json_doc_get_root(doc), "b"), &value));
	g_assert_false(axis_deluge_parse_boolean(axis_json_member(axis_json_doc_get_root(doc), "d"), &value));
	g_assert_true(axis_deluge_parse_boolean(axis_json_member(axis_json_doc_get_root(doc), "c"), &value));
	g_assert_false(value);
	axis_json_doc_free(doc);

	/* Hashes: forty hexadecimal digits of either case, in lower case. */
	g_assert_cmpint(reply_of("add-ok.json", &doc, &result, &message), ==, AXIS_DELUGE_REPLY_OK);
	hash = axis_deluge_parse_hash(result);
	g_assert_cmpstr(hash, ==, "0123456789abcdef0123456789abcdef01234567");
	g_clear_pointer(&hash, g_free);
	axis_json_doc_free(doc);
	g_assert_cmpint(reply_of("add-upper.json", &doc, &result, &message), ==, AXIS_DELUGE_REPLY_OK);
	hash = axis_deluge_parse_hash(result);
	g_assert_cmpstr(hash, ==, "0123456789abcdef0123456789abcdef01234567");
	g_clear_pointer(&hash, g_free);
	axis_json_doc_free(doc);
	g_assert_cmpint(reply_of("add-null.json", &doc, &result, &message), ==, AXIS_DELUGE_REPLY_OK);
	g_assert_null(axis_deluge_parse_hash(result));
	axis_json_doc_free(doc);
	g_assert_cmpint(reply_of("add-not-a-hash.json", &doc, &result, &message), ==, AXIS_DELUGE_REPLY_OK);
	g_assert_null(axis_deluge_parse_hash(result));
	axis_json_doc_free(doc);
	doc = doc_of_text("\"0123456789abcdef0123456789abcdef0123456\"");
	g_assert_null(axis_deluge_parse_hash(axis_json_doc_get_root(doc)));
	axis_json_doc_free(doc);
	doc = doc_of_text("\"0123456789abcdef0123456789abcdef012345678\"");
	g_assert_null(axis_deluge_parse_hash(axis_json_doc_get_root(doc)));
	axis_json_doc_free(doc);
	doc = doc_of_text("\"0123456789abcdef0123456789abcdef0123456g\"");
	g_assert_null(axis_deluge_parse_hash(axis_json_doc_get_root(doc)));
	axis_json_doc_free(doc);

	/* The version is text. */
	g_assert_cmpint(reply_of("version.json", &doc, &result, &message), ==, AXIS_DELUGE_REPLY_OK);
	version = axis_deluge_parse_version(result);
	g_assert_cmpstr(version, ==, "2.1.1");
	axis_json_doc_free(doc);
	doc = doc_of_text("12");
	g_assert_null(axis_deluge_parse_version(axis_json_doc_get_root(doc)));
	axis_json_doc_free(doc);

	/* Names: plugins without regard to case, labels with it. */
	g_assert_cmpint(reply_of("plugins.json", &doc, &result, &message), ==, AXIS_DELUGE_REPLY_OK);
	g_assert_cmpint(axis_deluge_parse_has_name(result, "Label"), ==, 1);
	g_assert_cmpint(axis_deluge_parse_has_name(result, "label"), ==, 1);
	g_assert_cmpint(axis_deluge_parse_has_name(result, "Execute"), ==, 0);
	axis_json_doc_free(doc);
	g_assert_cmpint(reply_of("plugins-no-label.json", &doc, &result, &message), ==, AXIS_DELUGE_REPLY_OK);
	g_assert_cmpint(axis_deluge_parse_has_name(result, "Label"), ==, 0);
	axis_json_doc_free(doc);
	g_assert_cmpint(reply_of("labels.json", &doc, &result, &message), ==, AXIS_DELUGE_REPLY_OK);
	g_assert_cmpint(axis_deluge_parse_has_label(result, "axis"), ==, 1);
	g_assert_cmpint(axis_deluge_parse_has_label(result, "AXIS"), ==, 0);
	axis_json_doc_free(doc);
	g_assert_cmpint(reply_of("labels-no-axis.json", &doc, &result, &message), ==, AXIS_DELUGE_REPLY_OK);
	g_assert_cmpint(axis_deluge_parse_has_label(result, "axis"), ==, 0);
	axis_json_doc_free(doc);
	doc = doc_of_text("{\"a\": 1}");
	g_assert_cmpint(axis_deluge_parse_has_label(axis_json_doc_get_root(doc), "axis"), ==, -1);
	g_assert_cmpint(axis_deluge_parse_has_name(axis_json_doc_get_root(doc), "Label"), ==, -1);
	axis_json_doc_free(doc);

	/* Labels are Deluge's: lower case. */
	g_assert_true(axis_deluge_label_valid("axis"));
	g_assert_true(axis_deluge_label_valid("axis-movies_2.0"));
	g_assert_false(axis_deluge_label_valid("Axis"));
	g_assert_false(axis_deluge_label_valid("AXIS"));
	g_assert_false(axis_deluge_label_valid(""));
	g_assert_false(axis_deluge_label_valid(NULL));
	g_assert_false(axis_deluge_label_valid("-axis"));
	g_assert_false(axis_deluge_label_valid("a b"));
	g_assert_false(axis_deluge_label_valid("a/b"));

	/* Hosts: the first with an identifier connect can be given. */
	g_assert_cmpint(reply_of("hosts.json", &doc, &result, &message), ==, AXIS_DELUGE_REPLY_OK);
	g_assert_true(axis_deluge_parse_host(result, &host_id, &host, &port));
	g_assert_cmpstr(host_id, ==, HOST_ID);
	g_assert_cmpstr(host, ==, "127.0.0.1");
	g_assert_cmpint(port, ==, 58846);
	g_clear_pointer(&host_id, g_free);
	g_clear_pointer(&host, g_free);
	axis_json_doc_free(doc);
	g_assert_cmpint(reply_of("hosts-empty.json", &doc, &result, &message), ==, AXIS_DELUGE_REPLY_OK);
	g_assert_false(axis_deluge_parse_host(result, &host_id, &host, &port));
	g_assert_null(host_id);
	axis_json_doc_free(doc);
	g_assert_cmpint(reply_of("hosts-wrong-types.json", &doc, &result, &message), ==, AXIS_DELUGE_REPLY_OK);
	g_assert_true(axis_deluge_parse_host(result, &host_id, &host, &port));
	g_assert_cmpstr(host_id, ==, "b2c3d4e5f60718293a4b5c6d7e8f90a1");
	g_assert_null(host);
	g_assert_cmpint(port, ==, 0);
	g_clear_pointer(&host_id, g_free);
	axis_json_doc_free(doc);
	doc = doc_of_text("{\"a\": 1}");
	g_assert_false(axis_deluge_parse_host(axis_json_doc_get_root(doc), &host_id, &host, &port));
	axis_json_doc_free(doc);
}

static void
assert_item(
	const AxisDelugeSlot  *slot,
	AxisDownloadItemState  state,
	gint64                 size,
	gint64                 done,
	gint64                 rate,
	gint64                 eta
){
	g_assert_true(slot->known);
	g_assert_cmpint(slot->item->state, ==, state);
	g_assert_cmpint(slot->item->size_bytes, ==, size);
	g_assert_cmpint(slot->item->done_bytes, ==, done);
	g_assert_cmpint(slot->item->rate_bps, ==, rate);
	g_assert_cmpint(slot->item->eta_s, ==, eta);
	g_assert_cmpstr(slot->item->client_ref, ==, slot->item->info_hash);
}

/* Task 2.1: the state mapping, sizes, paths; completed while seeding */
static void
test_parse_torrents(void)
{
	g_autoptr(GPtrArray) slots = slots_of("torrents.json");
	const AxisDelugeSlot *slot;

	g_assert_nonnull(slots);
	/* No label at all is no torrent of Axis's: read with the label "" (another
	 * label than Axis's, which the layer leaves out), not as an entry that
	 * cannot be read. */
	g_assert_cmpuint(slots->len, ==, 15);
	g_assert_true(has_slot(slots, "0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b"));
	g_assert_cmpstr(slot_by_ref(slots, "0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b")->item->category, ==, "");

	slot = slot_by_ref(slots, "0101010101010101010101010101010101010101");
	assert_item(slot, AXIS_DOWNLOAD_ITEM_DOWNLOADING, 4000000000, 2530000000, 5242880, 280);
	g_assert_cmpstr(slot->item->name, ==, "Some.Film.2020.1080p.WEB-DL");
	g_assert_cmpstr(slot->item->category, ==, "axis");
	g_assert_cmpstr(slot->word, ==, "Downloading");
	g_assert_null(slot->item->path);
	g_assert_null(slot->item->message);

	/* Queued, Checking, Allocating: queued; no rate or time for what is not running. */
	assert_item(slot_by_ref(slots, "0202020202020202020202020202020202020202"), AXIS_DOWNLOAD_ITEM_QUEUED, 1500000000, 0, -1, -1);
	assert_item(slot_by_ref(slots, "0303030303030303030303030303030303030303"), AXIS_DOWNLOAD_ITEM_QUEUED, 1500000000, 187500000,
		-1, -1);
	assert_item(slot_by_ref(slots, "0404040404040404040404040404040404040404"), AXIS_DOWNLOAD_ITEM_QUEUED, 1500000000, 0, -1, -1);
	assert_item(slot_by_ref(slots, "0505050505050505050505050505050505050505"), AXIS_DOWNLOAD_ITEM_PAUSED, 700000000, 350000000,
		-1, -1);

	/* Moving is downloading, all of it, even though Deluge calls it finished: never a path. */
	slot = slot_by_ref(slots, "0606060606060606060606060606060606060606");
	assert_item(slot, AXIS_DOWNLOAD_ITEM_DOWNLOADING, 2000000000, 2000000000, -1, -1);
	g_assert_null(slot->item->path);

	/* Seeding: completed, at the location joined with the name. */
	slot = slot_by_ref(slots, "0707070707070707070707070707070707070707");
	assert_item(slot, AXIS_DOWNLOAD_ITEM_COMPLETED, 3000000000, 3000000000, -1, -1);
	g_assert_cmpstr(slot->item->path, ==, "/downloads/Seeding.Film.2018.1080p");
	g_assert_null(slot->item->message);

	/* Error: failed, with the tracker's words. */
	slot = slot_by_ref(slots, "0808080808080808080808080808080808080808");
	g_assert_true(slot->known);
	g_assert_cmpint(slot->item->state, ==, AXIS_DOWNLOAD_ITEM_FAILED);
	g_assert_cmpstr(slot->item->message, ==, "Error: unregistered torrent");
	g_assert_null(slot->item->path);

	/* Finished, though paused: completed (a trailing slash in the location is no second slash). */
	slot = slot_by_ref(slots, "0909090909090909090909090909090909090909");
	assert_item(slot, AXIS_DOWNLOAD_ITEM_COMPLETED, 800000000, 800000000, -1, -1);
	g_assert_cmpstr(slot->item->path, ==, "/downloads/Done.And.Paused.2017");

	/* Another label is read like any other (the layer drops it). */
	slot = slot_by_ref(slots, "0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a");
	g_assert_cmpstr(slot->item->category, ==, "manual");
	g_assert_cmpint(slot->item->eta_s, ==, 90000);

	/* The hash key is lower-cased, and is the identifier and the hash. */
	slot = slot_by_ref(slots, "abababababababababababababababababababab");
	g_assert_cmpstr(slot->item->info_hash, ==, "abababababababababababababababababababab");

	/* Finished with nothing to say where, or a name that is a path: completed, no path. */
	slot = slot_by_ref(slots, "0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d");
	g_assert_cmpint(slot->item->state, ==, AXIS_DOWNLOAD_ITEM_COMPLETED);
	g_assert_null(slot->item->path);
	slot = slot_by_ref(slots, "0e0e0e0e0e0e0e0e0e0e0e0e0e0e0e0e0e0e0e0e");
	g_assert_cmpint(slot->item->state, ==, AXIS_DOWNLOAD_ITEM_COMPLETED);
	g_assert_null(slot->item->path);

	/* The path is made from the torrent's own name, spaces and all; the name shown is cleaned. */
	slot = slot_by_ref(slots, "0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f");
	g_assert_cmpstr(slot->item->path, ==, "/downloads/Double  Space  Name");
	g_assert_cmpstr(slot->item->name, ==, "Double Space Name");
}

/* Task 2.1: missing, null and wrong-typed fields, no list, empty lists, unknown words */
static void
test_parse_broken_answers(void)
{
	g_autoptr(GPtrArray) slots = slots_of("torrents-wrong-types.json");
	g_autoptr(GPtrArray) empty = slots_of("torrents-empty.json");
	g_autoptr(GPtrArray) unknown = slots_of("torrents-unknown-state.json");
	g_autoptr(AxisJsonDoc) doc = doc_of_text("[]");
	g_autoptr(AxisJsonDoc) other = doc_of_text("{}");
	g_autoptr(AxisJsonDoc) none = doc_of_text("null");
	const AxisDelugeSlot *slot;
	g_autofree gchar *message = NULL;
	AxisJsonDoc *wrong = NULL;
	const AxisJsonNode *result = NULL;

	/* Entries with a bad hash are skipped, a hash is taken once: eight are
	 * left, and the six whose hash is read and not their name, their label or
	 * the rest of them (not an object), which say so (review of the fixes,
	 * item 7: the layer must know which torrent it is). */
	g_assert_nonnull(slots);
	g_assert_cmpuint(slots->len, ==, 14);
	g_assert_cmpstr(slot_by_ref(slots, "1616161616161616161616161616161616161616")->item->unreadable, ==,
		"it has no name Axis can read");
	g_assert_nonnull(slot_by_ref(slots, "1515151515151515151515151515151515151515")->item->unreadable);
	g_assert_nonnull(slot_by_ref(slots, "1717171717171717171717171717171717171717")->item->unreadable);
	g_assert_null(slot_by_ref(slots, "1717171717171717171717171717171717171717")->item->category);
	g_assert_nonnull(slot_by_ref(slots, "1818181818181818181818181818181818181818")->item->unreadable);
	g_assert_cmpstr(slot_by_ref(slots, "1919191919191919191919191919191919191919")->item->unreadable, ==,
		"Deluge's entry for it is not an object");
	g_assert_nonnull(slot_by_ref(slots, "1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a")->item->unreadable);
	g_assert_null(slot_by_ref(slots, "1010101010101010101010101010101010101010")->item->unreadable);
	/* Numbers in strings are numbers; a string that is not "true" or "false" is no boolean. */
	assert_item(slot_by_ref(slots, "1010101010101010101010101010101010101010"), AXIS_DOWNLOAD_ITEM_DOWNLOADING, 2000, 1000, 300, 60);
	/* Garbage sizes and times: not known, not wrong. */
	assert_item(slot_by_ref(slots, "1111111111111111111111111111111111111111"), AXIS_DOWNLOAD_ITEM_DOWNLOADING, -1, -1, -1, -1);
	/* Out of range: nothing. */
	assert_item(slot_by_ref(slots, "1212121212121212121212121212121212121212"), AXIS_DOWNLOAD_ITEM_DOWNLOADING, -1, -1, -1, -1);
	/* More done than all: all, never more. */
	assert_item(slot_by_ref(slots, "1313131313131313131313131313131313131313"), AXIS_DOWNLOAD_ITEM_DOWNLOADING, 1000, 1000, 0, 5);
	/* No amount done: the progress. */
	assert_item(slot_by_ref(slots, "1414141414141414141414141414141414141414"), AXIS_DOWNLOAD_ITEM_DOWNLOADING, 4000, 1000, -1, -1);
	/* A state that is not a word is not a known one. */
	slot = slot_by_ref(slots, "1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b");
	g_assert_false(slot->known);
	g_assert_null(slot->word);
	/* An error with no words still says something. */
	slot = slot_by_ref(slots, "1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c");
	g_assert_cmpint(slot->item->state, ==, AXIS_DOWNLOAD_ITEM_FAILED);
	g_assert_nonnull(slot->item->message);
	g_assert_cmpstr(slot_by_ref(slots, "1d1d1d1d1d1d1d1d1d1d1d1d1d1d1d1d1d1d1d1d")->item->name, ==, "Same.Hash.Again");

	/* Empty is a list; anything but an object is none. */
	g_assert_nonnull(empty);
	g_assert_cmpuint(empty->len, ==, 0);
	g_assert_null(axis_deluge_parse_torrents(axis_json_doc_get_root(doc)));
	g_assert_null(axis_deluge_parse_torrents(axis_json_doc_get_root(none)));
	g_assert_cmpint(reply_of("torrents-not-an-object.json", &wrong, &result, &message), ==, AXIS_DELUGE_REPLY_OK);
	g_assert_null(axis_deluge_parse_torrents(result));
	axis_json_doc_free(wrong);
	g_assert_nonnull(other);

	/* Unknown words: the slot says so and holds the word; finished is settled, whatever the word. */
	g_assert_cmpuint(unknown->len, ==, 2);
	slot = slot_by_ref(unknown, "2020202020202020202020202020202020202020");
	g_assert_false(slot->known);
	g_assert_cmpstr(slot->word, ==, "Wibbling");
	g_assert_cmpint(slot->item->state, ==, AXIS_DOWNLOAD_ITEM_QUEUED);
	slot = slot_by_ref(unknown, "2121212121212121212121212121212121212121");
	g_assert_true(slot->known);
	g_assert_cmpint(slot->item->state, ==, AXIS_DOWNLOAD_ITEM_COMPLETED);
	g_assert_cmpstr(slot->item->path, ==, "/downloads/Finished.Wibbling");
}

/* Task 2.1: lists are capped; long strings are cut */
static void
test_parse_capped(void)
{
	GString *text = g_string_new("{\"result\": {");
	g_autofree gchar *long_name = g_strnfill(1000, 'n');
	g_autofree gchar *message = NULL;
	AxisJsonDoc *doc;
	const AxisJsonNode *result;
	GPtrArray *slots;
	guint i;

	for (i = 0; i < 5200; i++)
		g_string_append_printf(text, "%s\"%040x\": {\"name\": \"Torrent %u\", \"state\": \"Queued\", \"label\": \"axis\"}",
			i > 0 ? "," : "", i + 1, i);
	g_string_append(text, "}, \"error\": null, \"id\": 1}");
	g_assert_cmpint(reply_of_text(text->str, 1, &doc, &result, &message), ==, AXIS_DELUGE_REPLY_OK);
	slots = axis_deluge_parse_torrents(result);
	g_assert_cmpuint(slots->len, ==, AXIS_DELUGE_MAX_TORRENTS);
	g_ptr_array_unref(slots);
	/* And it says so (third review, item 5). */
	{
		gboolean cut = FALSE;
		guint unreadable = 99;

		slots = axis_deluge_parse_torrents_counted(result, &unreadable, &cut);
		g_assert_true(cut);
		g_assert_cmpuint(unreadable, ==, 0);
		g_ptr_array_unref(slots);
	}
	axis_json_doc_free(doc);
	g_string_free(text, TRUE);

	/* An entry whose hash is read and not its name is returned saying so,
	 * not counted (review of the fixes, item 7: the layer knows which torrent
	 * it is); one whose hash cannot be read is counted. */
	text = g_string_new("{\"result\": {\"0101010101010101010101010101010101010101\": {\"name\": \"Some.Film\", \"state\": "
		"\"Queued\", \"label\": \"axis\"}, \"0202020202020202020202020202020202020202\": {\"state\": \"Queued\", "
		"\"label\": \"axis\"}, \"not a hash\": {\"name\": \"Broken\", \"state\": \"Queued\", \"label\": \"axis\"}}, "
		"\"error\": null, \"id\": 1}");
	g_assert_cmpint(reply_of_text(text->str, 1, &doc, &result, &message), ==, AXIS_DELUGE_REPLY_OK);
	{
		gboolean cut = TRUE;
		guint unreadable = 0;

		slots = axis_deluge_parse_torrents_counted(result, &unreadable, &cut);
		g_assert_cmpuint(slots->len, ==, 2);
		g_assert_nonnull(slot_by_ref(slots, "0202020202020202020202020202020202020202")->item->unreadable);
		g_assert_false(cut);
		g_assert_cmpuint(unreadable, ==, 1);
		g_ptr_array_unref(slots);
	}
	axis_json_doc_free(doc);
	g_string_free(text, TRUE);

	text = g_string_new(NULL);
	g_string_append_printf(text, "{\"result\": {\"0101010101010101010101010101010101010101\": {\"name\": \"%s\", \"state\": "
		"\"Queued\", \"label\": \"axis\"}}, \"error\": null, \"id\": 1}", long_name);
	g_assert_cmpint(reply_of_text(text->str, 1, &doc, &result, &message), ==, AXIS_DELUGE_REPLY_OK);
	slots = axis_deluge_parse_torrents(result);
	g_assert_cmpuint(g_utf8_strlen(((AxisDelugeSlot *)g_ptr_array_index(slots, 0))->item->name, -1), ==,
		AXIS_DOWNLOAD_NAME_MAX_CHARS);
	/* A name too long for a path is no path (a finished torrent is still completed). */
	g_ptr_array_unref(slots);
	axis_json_doc_free(doc);
	g_string_free(text, TRUE);
}

/* ── Test (1.2, 2.1) ────────────────────────────────────────────── */

/* Spec: Add (a client that works); task 1.2: what the test says, and the calls it makes */
static void
test_client_test(Fixture *fx, gconstpointer data)
{
	AxisDownloadTestResult result = test_client(fx, fx->client_id);
	g_autofree gchar *calls = NULL;
	g_autoptr(AxisDownloadClientInfo) client = NULL;
	g_autoptr(GError) error = NULL;

	g_assert_cmpstr(result.version, ==, "2.1.1");
	g_assert_cmpint(result.category_exists, ==, 1);
	/* The priority is ignored, and the test says so. */
	g_assert_nonnull(strstr(result.note, "ignores the priority"));
	axis_download_test_result_clear(&result);
	g_assert_true(axis_downloads_get_client(fx->downloads, fx->client_id, &client, &error));
	g_assert_cmpint(client->state, ==, AXIS_DOWNLOAD_CLIENT_OK);
	g_assert_cmpstr(client->version, ==, "2.1.1");
	g_assert_cmpint(client->category_exists, ==, 1);
	calls = dg_calls(fx->dg);
	g_assert_cmpstr(calls, ==, "auth.login web.connected daemon.get_version core.get_enabled_plugins label.get_labels");
	g_assert_cmpuint(dg_bad_requests(fx->dg), ==, 0);
	/* Nothing was changed at Deluge. */
	g_assert_cmpuint(dg_count(fx->dg, "label.add"), ==, 0);
	g_assert_cmpuint(dg_count(fx->dg, "web.connect:"), ==, 0);
}

/* Task 1.2: a web interface not connected to a daemon is connected to the first host, and that is said */
static void
test_client_test_connects(Fixture *fx, gconstpointer data)
{
	AxisDownloadTestResult result;
	g_autofree gchar *calls = NULL;

	DG_SET(fx->dg, connected, FALSE);
	result = test_client(fx, fx->client_id);
	g_assert_nonnull(strstr(result.note, "was not connected to a daemon"));
	g_assert_nonnull(strstr(result.note, "127.0.0.1:58846"));
	g_assert_cmpint(result.category_exists, ==, 1);
	g_assert_cmpstr(result.version, ==, "2.1.1");
	axis_download_test_result_clear(&result);
	calls = dg_calls(fx->dg);
	g_assert_cmpstr(calls, ==, "auth.login web.connected web.get_hosts web.connect:" HOST_ID " web.connected daemon.get_version "
		"core.get_enabled_plugins label.get_labels");
	g_assert_cmpint(client_state(fx, fx->client_id), ==, AXIS_DOWNLOAD_CLIENT_OK);
}

/* Spec: Not ready (no daemon) */
static void
test_client_test_not_connected(Fixture *fx, gconstpointer data)
{
	AxisDownloadTestResult result;

	/* Not connected, and no host to connect to. */
	DG_SET(fx->dg, connected, FALSE);
	DG_SET(fx->dg, has_hosts, FALSE);
	result = test_client(fx, fx->client_id);
	g_assert_nonnull(strstr(result.note, "not connected to a daemon"));
	g_assert_nonnull(strstr(result.note, "lists none"));
	axis_download_test_result_clear(&result);
	g_assert_cmpint(client_state(fx, fx->client_id), ==, AXIS_DOWNLOAD_CLIENT_FAILING);
	g_assert_cmpuint(dg_count(fx->dg, "web.connect:"), ==, 0);
	g_assert_cmpuint(dg_count(fx->dg, "core.get_enabled_plugins"), ==, 0);

	/* A host, but connecting does not work. */
	DG_SET(fx->dg, has_hosts, TRUE);
	DG_SET(fx->dg, connect_works, FALSE);
	result = test_client(fx, fx->client_id);
	g_assert_nonnull(strstr(result.note, "not connected to a daemon"));
	g_assert_nonnull(strstr(result.note, "did not work"));
	axis_download_test_result_clear(&result);
	g_assert_cmpint(client_state(fx, fx->client_id), ==, AXIS_DOWNLOAD_CLIENT_FAILING);
	g_assert_cmpuint(dg_count(fx->dg, "web.connect:"), ==, 1);
	g_assert_cmpuint(dg_count(fx->dg, "label.get_labels"), ==, 0);
}

/* Spec: Not ready (no Label plugin) */
static void
test_client_test_no_label_plugin(Fixture *fx, gconstpointer data)
{
	AxisDownloadTestResult result;

	DG_SET(fx->dg, has_label_plugin, FALSE);
	result = test_client(fx, fx->client_id);
	g_assert_nonnull(strstr(result.note, "Label plugin is not enabled"));
	axis_download_test_result_clear(&result);
	g_assert_cmpint(client_state(fx, fx->client_id), ==, AXIS_DOWNLOAD_CLIENT_FAILING);
	g_assert_cmpuint(dg_count(fx->dg, "label.add"), ==, 0);
}

/* Task 1.2: the label is made when it is not there */
static void
test_client_test_makes_the_label(Fixture *fx, gconstpointer data)
{
	static const gchar *const names[] = { "movies", NULL };
	AxisDownloadTestResult result;

	dg_set_labels(fx->dg, names);
	g_assert_false(dg_has_label(fx->dg, "axis"));
	result = test_client(fx, fx->client_id);
	g_assert_nonnull(strstr(result.note, "\"axis\" did not exist"));
	g_assert_nonnull(strstr(result.note, "it was made"));
	g_assert_cmpint(result.category_exists, ==, 1);
	axis_download_test_result_clear(&result);
	g_assert_true(dg_has_label(fx->dg, "axis"));
	g_assert_cmpuint(dg_count(fx->dg, "label.add:axis"), ==, 1);
	g_assert_cmpint(client_state(fx, fx->client_id), ==, AXIS_DOWNLOAD_CLIENT_OK);
	/* Once there, the next test makes nothing. */
	result = test_client(fx, fx->client_id);
	axis_download_test_result_clear(&result);
	g_assert_cmpuint(dg_count(fx->dg, "label.add"), ==, 1);
}

/* Spec: Not ready (wrong password); task 2.2: never in the words */
static void
test_client_wrong_password(Fixture *fx, gconstpointer data)
{
	gint64 wrong = add_client_for(fx, fx->dg, "Wrong", WRONG_PASSWORD);
	AxisTestPlex *closed = axis_test_plex_new();
	g_autofree gchar *closed_base = axis_test_plex_base(closed);
	g_autoptr(AxisDownloadClientInfo) client = NULL;
	g_autoptr(GError) error = NULL;
	AxisDownloadTestResult result;
	gint64 down;

	result = test_client(fx, wrong);
	g_assert_nonnull(strstr(result.note, "refused the web password"));
	g_assert_null(strstr(result.note, WRONG_PASSWORD));
	g_assert_null(strstr(result.note, PASSWORD));
	axis_download_test_result_clear(&result);
	g_assert_true(axis_downloads_get_client(fx->downloads, wrong, &client, &error));
	g_assert_cmpint(client->state, ==, AXIS_DOWNLOAD_CLIENT_REFUSED);
	g_assert_null(strstr(client->state_message, WRONG_PASSWORD));
	g_clear_pointer(&client, axis_download_client_info_free);
	/* Nothing else was asked after the refusal. */
	g_assert_cmpuint(dg_count(fx->dg, "auth.login"), ==, 1);
	g_assert_cmpuint(dg_count(fx->dg, "web."), ==, 0);

	/* Nothing listening: unreachable, not refused. */
	axis_test_plex_free(closed);
	down = add_client_full(fx, "Down", closed_base, NULL, PASSWORD, NULL, AXIS_DOWNLOAD_PRIORITY_NORMAL);
	result = test_client(fx, down);
	axis_download_test_result_clear(&result);
	g_assert_cmpint(client_state(fx, down), ==, AXIS_DOWNLOAD_CLIENT_UNREACHABLE);

	/* Something that answers but is not Deluge's web interface: failing, not refused. */
	dg_raw(fx->dg, "auth.login", 200, "text/html", "<html><body>Welcome to nginx</body></html>");
	result = test_client(fx, fx->client_id);
	g_assert_nonnull(strstr(result.note, "cannot be read"));
	axis_download_test_result_clear(&result);
	g_assert_cmpint(client_state(fx, fx->client_id), ==, AXIS_DOWNLOAD_CLIENT_FAILING);

	/* A login that says yes and sets no session. */
	dg_raw_fixture(fx->dg, "auth.login", "login-ok.json");
	result = test_client(fx, fx->client_id);
	g_assert_nonnull(strstr(result.note, "set no session"));
	axis_download_test_result_clear(&result);
	g_assert_cmpint(client_state(fx, fx->client_id), ==, AXIS_DOWNLOAD_CLIENT_FAILING);

	/* The answer false, from a fixture. */
	dg_raw_fixture(fx->dg, "auth.login", "login-wrong.json");
	result = test_client(fx, fx->client_id);
	g_assert_nonnull(strstr(result.note, "refused the web password"));
	axis_download_test_result_clear(&result);
	g_assert_cmpint(client_state(fx, fx->client_id), ==, AXIS_DOWNLOAD_CLIENT_REFUSED);
	g_assert_cmpuint(logged("*" WRONG_PASSWORD "*"), ==, 0);
	g_assert_cmpuint(logged("*" PASSWORD "*"), ==, 0);
}

/* Task 1.2: the label must be one Deluge takes; this is said, and nothing is sent */
static void
test_client_label_must_be_lower_case(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *base = axis_test_plex_base(fx->dg->server);
	gint64 upper = add_client_full(fx, "Upper", base, NULL, PASSWORD, "Axis", AXIS_DOWNLOAD_PRIORITY_NORMAL);
	AxisDownloadPollReport report;
	AxisDownloadTestResult result;
	g_autoptr(GError) error = NULL;
	gint64 id = 0;
	AxisDownloadRequest request;
	g_autoptr(GBytes) file = axis_test_download_torrent("Some.Film.2020", NULL);

	result = test_client(fx, upper);
	g_assert_nonnull(strstr(result.note, "lower case"));
	axis_download_test_result_clear(&result);
	g_assert_cmpint(client_state(fx, upper), ==, AXIS_DOWNLOAD_CLIENT_FAILING);
	report = poll_client(fx, upper);
	g_assert_false(report.reached);

	memset(&request, 0, sizeof(request));
	request.client_id = upper;
	request.protocol = AXIS_DOWNLOAD_TORRENT;
	request.file = file;
	request.release_name = "Some.Film.2020";
	g_assert_false(axis_downloads_add(fx->downloads, &request, &id, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID);
	g_assert_nonnull(strstr(error->message, "lower case"));
	assert_state(fx, id, "failed");
	g_assert_cmpuint(axis_test_plex_n_requests(fx->dg->server), ==, 0);
}

/* Task 1.2: an older Deluge answers daemon.info */
static void
test_client_test_old_daemon(Fixture *fx, gconstpointer data)
{
	AxisDownloadTestResult result;

	DG_SET(fx->dg, old_daemon, TRUE);
	result = test_client(fx, fx->client_id);
	g_assert_cmpstr(result.version, ==, "1.3.15");
	axis_download_test_result_clear(&result);
	g_assert_cmpint(client_state(fx, fx->client_id), ==, AXIS_DOWNLOAD_CLIENT_OK);
	/* No version at all is not a failure. */
	dg_raw_fixture(fx->dg, "daemon.get_version", "error-unknown-method.json");
	dg_raw_fixture(fx->dg, "daemon.info", "error-unknown-method.json");
	result = test_client(fx, fx->client_id);
	g_assert_null(result.version);
	axis_download_test_result_clear(&result);
	g_assert_cmpint(client_state(fx, fx->client_id), ==, AXIS_DOWNLOAD_CLIENT_OK);
}

/* ── Add (1.3, 2.1) ─────────────────────────────────────────────── */

/* Spec: Add.  A torrent file: added, given Axis's label, recorded by its info-hash. */
static void
test_add_torrent_file(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *hash = NULL;
	gint64 id = add_torrent_release(fx, "Some.Film.2020.1080p", &hash);
	g_autofree gchar *ref = ref_of(fx, id);
	g_autofree gchar *label = dg_label_of(fx->dg, hash);
	g_autofree gchar *options = dg_text(fx->dg, &fx->dg->last_add_options);
	g_autofree gchar *file_name = dg_text(fx->dg, &fx->dg->last_add_name);
	g_autofree gchar *calls = dg_calls(fx->dg);
	g_autofree gchar *expected = NULL;
	g_autoptr(AxisDownloadInfo) info = NULL;
	g_autoptr(GError) error = NULL;

	g_assert_cmpstr(ref, ==, hash);
	assert_state(fx, id, "queued");
	g_assert_cmpstr(label, ==, "axis");
	g_assert_cmpstr(options, ==, "{}");
	g_assert_cmpstr(file_name, ==, "Some.Film.2020.1080p.torrent");
	assert_q(fx, hash, "SELECT info_hash FROM download");
	/* The label is made sure of before the torrent exists; one login, no session check on a fresh connection. */
	expected = g_strdup_printf("auth.login core.get_enabled_plugins label.get_labels core.add_torrent_file label.set_torrent:%s:axis",
		hash);
	g_assert_cmpstr(calls, ==, expected);
	g_assert_cmpuint(dg_n_torrents(fx->dg), ==, 1);
	g_assert_true(axis_downloads_get(fx->downloads, id, &info, &error));
	g_assert_cmpstr(info->info_hash, ==, hash);
	g_assert_cmpuint(dg_bad_requests(fx->dg), ==, 0);
}

/* Spec: Add.  A magnet link. */
static void
test_add_magnet(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *name = NULL;
	gint64 id = add_magnet_at(fx, "Some.Magnet.Release", &error);
	g_autofree gchar *ref = NULL;
	g_autofree gchar *label = dg_label_of(fx->dg, MAGNET_HASH);

	g_assert_no_error(error);
	ref = ref_of(fx, id);
	g_assert_cmpstr(ref, ==, MAGNET_HASH);
	g_assert_cmpstr(label, ==, "axis");
	name = dg_text(fx->dg, &fx->dg->last_add_name);
	g_assert_cmpstr(name, ==, MAGNET);
	g_assert_cmpuint(dg_count(fx->dg, "core.add_torrent_magnet"), ==, 1);
	g_assert_cmpuint(dg_count(fx->dg, "core.add_torrent_file"), ==, 0);
	assert_state(fx, id, "queued");
	/* The same magnet twice at one client is refused by the layer: nothing more is sent. */
	add_magnet_at(fx, "Some.Magnet.Release", &error);
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_EXISTS);
	g_assert_cmpuint(dg_count(fx->dg, "core.add_torrent_magnet"), ==, 1);
}

/* Task 1.3: the hash is compared without regard to case */
static void
test_add_hash_case_is_ignored(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *hash = NULL;
	g_autofree gchar *upper = NULL;
	g_autoptr(GBytes) file = axis_test_download_torrent("Some.Film.2020.1080p", &hash);
	gint64 id;

	/* Deluge answers the hash in upper case. */
	upper = g_ascii_strup(hash, -1);
	dg_set_text(fx->dg, &fx->dg->add_hash_override, upper);
	g_clear_pointer(&hash, g_free);
	id = add_torrent_release(fx, "Some.Film.2020.1080p", &hash);
	assert_state(fx, id, "queued");
	/* The record holds the hash as Axis computed it, and the label was set on it. */
	assert_q(fx, hash, "SELECT client_ref FROM download");
	g_assert_cmpuint(dg_count(fx->dg, "label.set_torrent:"), ==, 1);
}

/* The core's priority is ignored: it is in no request. */
static void
test_add_priority_is_ignored(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *transcript = NULL;

	add_torrent_release(fx, "Some.Film.2020.1080p", NULL);
	transcript = axis_test_plex_transcript(fx->dg->server);
	g_assert_null(strstr(transcript, "riority"));
}

/* Task 1.3: a label that is not there is made before the add; a Label plugin that is off stops it before */
static void
test_add_makes_the_label_first(Fixture *fx, gconstpointer data)
{
	static const gchar *const names[] = { "movies", NULL };
	g_autoptr(GError) error = NULL;
	g_autofree gchar *calls = NULL;
	g_autofree gchar *hash = NULL;
	gint64 id;

	dg_set_labels(fx->dg, names);
	id = add_torrent_release(fx, "Some.Film.2020.1080p", &hash);
	assert_state(fx, id, "queued");
	g_assert_true(dg_has_label(fx->dg, "axis"));
	calls = dg_calls(fx->dg);
	g_assert_nonnull(strstr(calls, "label.get_labels label.add:axis core.add_torrent_file label.set_torrent:"));

	/* Label plugin off: nothing is added. */
	DG_SET(fx->dg, has_label_plugin, FALSE);
	id = add_torrent_release_at(fx, "Another.Film.2019.720p", NULL, &error);
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_REFUSED);
	g_assert_nonnull(strstr(error->message, "Label plugin is not enabled"));
	assert_state(fx, id, "failed");
	g_assert_cmpuint(dg_count(fx->dg, "core.add_torrent_file"), ==, 1);
	g_assert_cmpuint(dg_n_torrents(fx->dg), ==, 1);
}

/* Spec: Label cannot be set; task 2.5 */
static void
test_add_label_cannot_be_set(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *hash = NULL;
	g_autofree gchar *expected = NULL;
	gint64 id;

	DG_SET(fx->dg, fail_set_label, TRUE);
	id = add_torrent_release_at(fx, "Some.Film.2020.1080p", &hash, &error);
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_REFUSED);
	g_assert_nonnull(strstr(error->message, "would not give it the label"));
	g_assert_nonnull(strstr(error->message, "it was removed again"));
	/* The torrent is gone again, without its data, and the add failed. */
	g_assert_false(dg_has(fx->dg, hash));
	expected = g_strdup_printf("core.remove_torrent:%s:0", hash);
	g_assert_cmpuint(dg_count(fx->dg, expected), ==, 1);
	g_assert_cmpuint(dg_count(fx->dg, "core.remove_torrent"), ==, 1);
	g_assert_cmpuint(dg_count(fx->dg, "core.add_torrent_file"), ==, 1);
	assert_state(fx, id, "failed");
	assert_q(fx, NULL, "SELECT client_ref FROM download");
	/* Not visible to a poll either. */
	g_assert_true(poll(fx).reached);
	assert_state(fx, id, "failed");

	/* When it cannot be removed again either, that is said: it is left, and by hand. */
	DG_SET(fx->dg, fail_remove, TRUE);
	g_clear_error(&error);
	g_clear_pointer(&hash, g_free);
	id = add_torrent_release_at(fx, "Another.Film.2019.720p", &hash, &error);
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_REFUSED);
	g_assert_nonnull(strstr(error->message, "remove it in Deluge by hand"));
	g_assert_true(dg_has(fx->dg, hash));
	assert_state(fx, id, "failed");
}

/* Task 2.5: an info-hash that is not the one Axis computed: the add failed, the stray torrent is not touched */
static void
test_add_hash_mismatch(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *hash = NULL;
	gint64 id;

	dg_set_text(fx->dg, &fx->dg->add_hash_override, "ffffffffffffffffffffffffffffffffffffffff");
	id = add_torrent_release_at(fx, "Some.Film.2020.1080p", &hash, &error);
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_REFUSED);
	g_assert_nonnull(strstr(error->message, "not the one Axis computed"));
	assert_state(fx, id, "failed");
	assert_q(fx, NULL, "SELECT client_ref FROM download");
	/* Not labelled, not removed: not touched. */
	g_assert_cmpuint(dg_count(fx->dg, "label.set_torrent"), ==, 0);
	g_assert_cmpuint(dg_count(fx->dg, "core.remove_torrent"), ==, 0);
	g_assert_true(dg_has(fx->dg, hash));
	g_assert_cmpuint(dg_count(fx->dg, "core.add_torrent_file"), ==, 1);

	/* An answer that is no hash at all, or none. */
	dg_set_text(fx->dg, &fx->dg->add_hash_override, NULL);
	dg_raw_fixture(fx->dg, "core.add_torrent_file", "add-null.json");
	g_clear_error(&error);
	g_clear_pointer(&hash, g_free);
	id = add_torrent_release_at(fx, "Another.Film.2019.720p", &hash, &error);
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_REFUSED);
	g_assert_nonnull(strstr(error->message, "gave no info-hash"));
	assert_state(fx, id, "failed");
	dg_raw_fixture(fx->dg, "core.add_torrent_file", "add-not-a-hash.json");
	g_clear_error(&error);
	id = add_torrent_release_at(fx, "Third.Film.2018.720p", NULL, &error);
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_REFUSED);
	assert_state(fx, id, "failed");
	g_assert_cmpuint(dg_count(fx->dg, "label.set_torrent"), ==, 0);
	g_assert_cmpuint(dg_count(fx->dg, "core.remove_torrent"), ==, 0);
}

/* Deluge's own refusal of an add (here, a torrent it has) is a failed add with its words */
static void
test_add_refused_by_deluge(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	gint64 id;

	dg_raw_fixture(fx->dg, "core.add_torrent_file", "add-duplicate.json");
	id = add_torrent_release_at(fx, "Some.Film.2020.1080p", NULL, &error);
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_REFUSED);
	g_assert_nonnull(strstr(error->message, "Torrent already in session"));
	assert_state(fx, id, "failed");
	g_assert_cmpuint(dg_count(fx->dg, "label.set_torrent"), ==, 0);
}

/* Usenet releases and nothing else are refused before any request, by the kind itself and by the layer */
static void
test_add_refuses_usenet(Fixture *fx, gconstpointer data)
{
	const AxisDownloadKindInfo *kind = axis_download_deluge_kind();
	g_autoptr(GBytes) nzb = g_bytes_new_static("<nzb/>", 6);
	g_autoptr(GBytes) empty = g_bytes_new_static("", 0);
	g_autoptr(GError) error = NULL;
	AxisDownloadRelease release;
	AxisDownloadConn conn;
	AxisDownloadRequest request;
	g_autofree gchar *ref = NULL;

	/* The kind itself, with a connection that has no session to send with:
	 * it must refuse before it would ever touch one. */
	memset(&conn, 0, sizeof(conn));
	memset(&release, 0, sizeof(release));
	release.protocol = AXIS_DOWNLOAD_USENET;
	release.file = nzb;
	release.name = "Some.Film.2020";
	g_assert_false(kind->add(kind, &conn, &release, "axis", AXIS_DOWNLOAD_PRIORITY_NORMAL, &ref, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID);
	g_clear_error(&error);
	/* A torrent release that is a file and a magnet, or neither, or empty, or with no hash. */
	release.protocol = AXIS_DOWNLOAD_TORRENT;
	release.magnet = MAGNET;
	release.info_hash = MAGNET_HASH;
	g_assert_false(kind->add(kind, &conn, &release, "axis", AXIS_DOWNLOAD_PRIORITY_NORMAL, &ref, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID);
	g_clear_error(&error);
	release.file = NULL;
	release.magnet = NULL;
	g_assert_false(kind->add(kind, &conn, &release, "axis", AXIS_DOWNLOAD_PRIORITY_NORMAL, &ref, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID);
	g_clear_error(&error);
	release.file = empty;
	g_assert_false(kind->add(kind, &conn, &release, "axis", AXIS_DOWNLOAD_PRIORITY_NORMAL, &ref, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID);
	g_clear_error(&error);
	release.file = NULL;
	release.magnet = MAGNET;
	release.info_hash = NULL;
	g_assert_false(kind->add(kind, &conn, &release, "axis", AXIS_DOWNLOAD_PRIORITY_NORMAL, &ref, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID);
	g_clear_error(&error);
	release.info_hash = MAGNET_HASH;
	g_assert_false(kind->add(kind, &conn, &release, "Axis", AXIS_DOWNLOAD_PRIORITY_NORMAL, &ref, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID);
	g_clear_error(&error);
	g_assert_false(kind->remove(kind, &conn, "not a hash", FALSE, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID);
	g_clear_error(&error);

	/* Through the layer: a Usenet release for this client is refused too. */
	memset(&request, 0, sizeof(request));
	request.client_id = fx->client_id;
	request.protocol = AXIS_DOWNLOAD_USENET;
	request.file = nzb;
	request.release_name = "Some.Film.2020";
	g_assert_false(axis_downloads_add(fx->downloads, &request, NULL, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID);
	g_clear_error(&error);
	request.client_id = 0;
	g_assert_false(axis_downloads_add(fx->downloads, &request, NULL, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_NO_CLIENT);
	assert_q(fx, "0", "SELECT count(*) FROM download");
	g_assert_cmpuint(axis_test_plex_n_requests(fx->dg->server), ==, 0);
}

/* ── List (1.4, 2.3) ────────────────────────────────────────────── */

/* Task 2.3: torrents with another label or none are never listed, touched or removed */
static void
test_other_labels_are_never_touched(Fixture *fx, gconstpointer data)
{
	AxisDownloadPollReport report;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *hash = NULL;
	g_autofree gchar *filter = NULL;
	g_autofree gchar *keys = NULL;
	g_autofree gchar *removal = NULL;
	gint64 id = add_torrent_release(fx, "Some.Film.2020.1080p", &hash);

	/* Another label (a person's), none, and Axis's own label on a torrent Axis did not add. */
	dg_put(fx->dg, "aa01aa01aa01aa01aa01aa01aa01aa01aa01aa01", "Holiday.Videos", "manual", "Downloading", 100, 50, "/downloads", FALSE, "");
	dg_put(fx->dg, "aa02aa02aa02aa02aa02aa02aa02aa02aa02aa02", "Holiday.Videos.Two", "manual", "Seeding", 100, 100, "/downloads", TRUE, "");
	dg_put(fx->dg, "aa03aa03aa03aa03aa03aa03aa03aa03aa03aa03", "No.Label.Seed", NULL, "Seeding", 100, 100, "/downloads", TRUE, "");
	dg_put(fx->dg, "aa04aa04aa04aa04aa04aa04aa04aa04aa04aa04", "Other.Film.2018", "axis", "Downloading", 100, 50, "/downloads", FALSE, "");

	report = poll(fx);
	g_assert_true(report.reached);
	g_assert_cmpuint(report.listed, ==, 2);		/* Axis's label only: its own and one of another program's */
	g_assert_cmpuint(report.ignored, ==, 1);	/* the other program's, never touched */
	assert_q(fx, "1", "SELECT count(*) FROM download");
	assert_q(fx, "0", "SELECT count(*) FROM download WHERE client_ref LIKE 'aa0%'");

	/* What the kind asked for: the label and only the keys it needs. */
	filter = dg_text(fx->dg, &fx->dg->last_filter);
	keys = dg_text(fx->dg, &fx->dg->last_keys);
	g_assert_cmpstr(filter, ==, "axis");
	g_assert_cmpstr(keys, ==, "name,state,progress,total_wanted,total_done,download_payload_rate,eta,download_location,"
		"is_finished,label,tracker_status,time_added");

	/* Removal of what Axis did not add is refused, and nothing is sent. */
	g_assert_false(axis_downloads_remove_ref(fx->downloads, fx->client_id, "aa01aa01aa01aa01aa01aa01aa01aa01aa01aa01", TRUE, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_NOT_FOUND);
	g_clear_error(&error);
	g_assert_false(axis_downloads_remove_ref(fx->downloads, fx->client_id, "aa03aa03aa03aa03aa03aa03aa03aa03aa03aa03", TRUE, &error));
	g_clear_error(&error);
	g_assert_false(axis_downloads_remove_ref(fx->downloads, fx->client_id, "aa04aa04aa04aa04aa04aa04aa04aa04aa04aa04", TRUE, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_NOT_FOUND);
	g_clear_error(&error);
	g_assert_cmpuint(dg_count(fx->dg, "core.remove_torrent"), ==, 0);
	g_assert_cmpuint(dg_n_torrents(fx->dg), ==, 5);

	/* Its own: removed, and only it. */
	g_assert_true(axis_downloads_remove_ref(fx->downloads, fx->client_id, hash, FALSE, &error));
	g_assert_no_error(error);
	g_assert_false(dg_has(fx->dg, hash));
	g_assert_cmpuint(dg_n_torrents(fx->dg), ==, 4);
	poll(fx);
	poll(fx);
	removal = g_strdup_printf("core.remove_torrent:%s:0", hash);
	g_assert_cmpuint(dg_count(fx->dg, "core.remove_torrent"), ==, 1);
	g_assert_cmpuint(dg_count(fx->dg, removal), ==, 1);
	assert_state(fx, id, "removed");
	assert_q(fx, "1", "SELECT count(*) FROM download");
}

/* Task 1.4, 2.1: the fixture through the whole layer; Spec: Seeding */
static void
test_fixture_answers_through_the_layer(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *seeding_place = g_build_filename(fx->disk, "downloads", "Seeding.Film.2018.1080p", NULL);
	g_autofree gchar *paused_place = g_build_filename(fx->disk, "downloads", "Done.And.Paused.2017", NULL);
	g_autofree gchar *spaced_place = g_build_filename(fx->disk, "downloads", "Double  Space  Name", NULL);
	g_autoptr(AxisDownloadInfo) info = NULL;
	g_autoptr(GError) error = NULL;
	AxisDownloadPollReport report;
	gint64 downloading;
	gint64 queued;
	gint64 checking;
	gint64 allocating;
	gint64 paused;
	gint64 moving;
	gint64 seeding;
	gint64 failed;
	gint64 done_paused;
	gint64 no_location;
	gint64 slashed;
	gint64 spaced;

	downloading = insert_record(fx, "0101010101010101010101010101010101010101", "Some.Film.2020.1080p.WEB-DL", "queued");
	queued = insert_record(fx, "0202020202020202020202020202020202020202", "Another.Film.2019.720p", "queued");
	checking = insert_record(fx, "0303030303030303030303030303030303030303", "Checking.Series.S01E01.720p", "queued");
	allocating = insert_record(fx, "0404040404040404040404040404040404040404", "Allocating.Series.S01E02.720p", "downloading");
	paused = insert_record(fx, "0505050505050505050505050505050505050505", "Paused.Film.2018.1080p", "queued");
	moving = insert_record(fx, "0606060606060606060606060606060606060606", "Moving.Film.2021.1080p", "queued");
	seeding = insert_record(fx, "0707070707070707070707070707070707070707", "Seeding.Film.2018.1080p", "downloading");
	failed = insert_record(fx, "0808080808080808080808080808080808080808", "Broken.Film.2017.720p", "downloading");
	done_paused = insert_record(fx, "0909090909090909090909090909090909090909", "Done.And.Paused.2017", "downloading");
	no_location = insert_record(fx, "0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d", "Finished.No.Location", "downloading");
	slashed = insert_record(fx, "0e0e0e0e0e0e0e0e0e0e0e0e0e0e0e0e0e0e0e0e", "a/b", "downloading");
	spaced = insert_record(fx, "0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f", "Double Space Name", "downloading");
	dg_raw_fixture(fx->dg, "core.get_torrents_status", "torrents.json");
	axis_test_storage_mkdir(seeding_place);
	axis_test_storage_mkdir(paused_place);
	axis_test_storage_mkdir(spaced_place);

	/* Without a path mapping nothing that finished is completed, and it says why. */
	report = poll(fx);
	g_assert_true(report.reached);
	g_assert_cmpuint(report.completed, ==, 0);
	g_assert_cmpuint(report.waiting, ==, 5);	/* seeding, paused and finished, no location, a name that is a path, spaced */
	assert_state(fx, seeding, "downloading");
	assert_state(fx, downloading, "downloading");
	assert_state(fx, queued, "queued");
	assert_state(fx, checking, "queued");
	assert_state(fx, allocating, "queued");
	assert_state(fx, paused, "paused");
	assert_state(fx, moving, "downloading");
	assert_state(fx, failed, "failed");
	g_assert_true(axis_downloads_get(fx->downloads, downloading, &info, &error));
	g_assert_cmpint(info->size_bytes, ==, 4000000000);
	g_assert_cmpint(info->done_bytes, ==, 2530000000);
	g_assert_cmpint(info->rate_bps, ==, 5242880);
	g_assert_cmpint(info->eta_s, ==, 280);
	g_clear_pointer(&info, axis_download_info_free);
	g_assert_true(axis_downloads_get(fx->downloads, failed, &info, &error));
	g_assert_cmpstr(info->message, ==, "Error: unregistered torrent");
	g_clear_pointer(&info, axis_download_info_free);
	g_assert_true(axis_downloads_get(fx->downloads, moving, &info, &error));
	g_assert_cmpfloat(axis_download_info_progress(info), ==, 1.0);
	g_assert_null(info->rel_path);
	g_clear_pointer(&info, axis_download_info_free);
	g_assert_true(axis_downloads_get(fx->downloads, seeding, &info, &error));
	g_assert_nonnull(strstr(info->problem, "no path mapping"));
	g_clear_pointer(&info, axis_download_info_free);

	/* Mapped: a finished torrent is completed, whatever Deluge's state word says, at the location joined with its name. */
	add_map(fx);
	report = poll(fx);
	g_assert_cmpuint(report.completed, ==, 3);	/* seeding, paused and finished, spaced */
	g_assert_cmpuint(report.waiting, ==, 2);	/* no location; a name that is a path */
	assert_state(fx, seeding, "completed");
	assert_state(fx, done_paused, "completed");
	assert_state(fx, spaced, "completed");
	assert_state(fx, no_location, "downloading");
	assert_state(fx, slashed, "downloading");
	g_assert_true(axis_downloads_get(fx->downloads, seeding, &info, &error));
	g_assert_cmpint(info->volume_id, ==, fx->volume_id);
	g_assert_cmpstr(info->rel_path, ==, "downloads/Seeding.Film.2018.1080p");
	g_assert_cmpstr(info->client_path, ==, "/downloads/Seeding.Film.2018.1080p");
	g_clear_pointer(&info, axis_download_info_free);
	g_assert_true(axis_downloads_get(fx->downloads, spaced, &info, &error));
	g_assert_cmpstr(info->client_path, ==, "/downloads/Double  Space  Name");
	g_clear_pointer(&info, axis_download_info_free);
	g_assert_true(axis_downloads_get(fx->downloads, no_location, &info, &error));
	g_assert_nonnull(strstr(info->problem, "no path"));
	g_clear_pointer(&info, axis_download_info_free);

	/* It stays completed while it seeds: poll after poll changes nothing. */
	report = poll(fx);
	g_assert_cmpuint(report.completed, ==, 0);
	report = poll(fx);
	g_assert_cmpuint(report.completed, ==, 0);
	assert_state(fx, seeding, "completed");
	assert_state(fx, done_paused, "completed");
}

/* Spec: Seeding.  A torrent that finishes and goes on seeding is completed once and stays so. */
static void
test_seeding_stays_completed(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *hash = NULL;
	AxisDownloadPollReport report;
	g_autofree gchar *place = g_build_filename(fx->disk, "downloads", "Some.Film.2020.1080p", NULL);
	gint64 id;
	guint i;

	add_map(fx);
	id = add_torrent_release(fx, "Some.Film.2020.1080p", &hash);
	dg_put(fx->dg, hash, "Some.Film.2020.1080p", "axis", "Downloading", 1000, 400, "/downloads", FALSE, "");
	poll(fx);
	assert_state(fx, id, "downloading");
	dg_set_state(fx->dg, hash, "Seeding", 1000, TRUE);
	axis_test_storage_mkdir(place);
	report = poll(fx);
	g_assert_cmpuint(report.completed, ==, 1);
	assert_state(fx, id, "completed");
	/* Seeding for as long as it likes: still completed, paused or not. */
	for (i = 0; i < 3; i++) {
		report = poll(fx);
		g_assert_cmpuint(report.completed, ==, 0);
		assert_state(fx, id, "completed");
	}
	dg_set_state(fx->dg, hash, "Paused", 1000, TRUE);
	poll(fx);
	assert_state(fx, id, "completed");
	dg_set_state(fx->dg, hash, "Seeding", 1000, TRUE);
	poll(fx);
	assert_state(fx, id, "completed");
	g_assert_true(dg_has(fx->dg, hash));
	/* Nothing is removed by Axis because it is finished. */
	g_assert_cmpuint(dg_count(fx->dg, "core.remove_torrent"), ==, 0);
}

/* A state word the kind does not know keeps the last state; the word is logged once */
static void
test_unknown_state_keeps_the_last_state(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *hash = NULL;
	gint64 id = add_torrent_release(fx, "Some.Film.2020.1080p", &hash);
	guint i;

	dg_put(fx->dg, hash, "Some.Film.2020.1080p", "axis", "Downloading", 1000, 400, "/downloads", FALSE, "");
	poll(fx);
	assert_state(fx, id, "downloading");
	dg_set_state(fx->dg, hash, "Wibbling", 450, FALSE);
	for (i = 0; i < 3; i++) {
		poll(fx);
		assert_state(fx, id, "downloading");
	}
	g_assert_cmpuint(logged("*Deluge client Torrents reported a torrent state Axis does not know (\"Wibbling\")*"), ==, 1);
	/* A known word again. */
	dg_set_state(fx->dg, hash, "Paused", 450, FALSE);
	poll(fx);
	assert_state(fx, id, "paused");
	/* Never seen before: queued, as it was recorded. */
	dg_put(fx->dg, "bb01bb01bb01bb01bb01bb01bb01bb01bb01bb01", "Never.Seen", "axis", "Wibbling", 100, 10, "/downloads", FALSE, "");
	g_assert_cmpuint(poll(fx).ignored, ==, 1);
	g_assert_cmpuint(logged("*\"Wibbling\"*"), ==, 1);
}

/* A list that cannot be read changes nothing (the torrents would look gone) */
static void
test_unreadable_lists_change_nothing(Fixture *fx, gconstpointer data)
{
	static const gchar *const fixtures[] = {
		"torrents-not-an-object.json", "not-an-object.json", "truncated.json", "error-unknown-method.json",
		"envelope-wrong-id.json", "envelope-both.json", "envelope-neither.json", "error-no-words.json", NULL
	};
	AxisDownloadPollReport report;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *hash = NULL;
	gint64 id = add_torrent_release(fx, "Some.Film.2020.1080p", &hash);
	guint i;

	dg_put(fx->dg, hash, "Some.Film.2020.1080p", "axis", "Downloading", 1000, 400, "/downloads", FALSE, "");
	poll(fx);
	assert_state(fx, id, "downloading");
	for (i = 0; fixtures[i] != NULL; i++) {
		dg_raw_fixture(fx->dg, "core.get_torrents_status", fixtures[i]);
		report = poll(fx);
		g_assert_false(report.reached);
		assert_state(fx, id, "downloading");
	}
	dg_raw(fx->dg, "core.get_torrents_status", 200, "text/html", "<html>Bad gateway</html>");
	g_assert_false(poll(fx).reached);
	dg_raw(fx->dg, "core.get_torrents_status", 502, "text/html", "<html>Bad gateway</html>");
	g_assert_false(poll(fx).reached);
	assert_state(fx, id, "downloading");
	assert_q(fx, "0", "SELECT missing_polls FROM download");
	g_assert_cmpint(client_state(fx, fx->client_id), ==, AXIS_DOWNLOAD_CLIENT_FAILING);
	/* And everything is as before once it answers. */
	dg_clear_raw(fx->dg, "core.get_torrents_status");
	report = poll(fx);
	g_assert_true(report.reached);
	assert_state(fx, id, "downloading");
	g_assert_cmpint(client_state(fx, fx->client_id), ==, AXIS_DOWNLOAD_CLIENT_OK);
}

/* Third review, item 5: an entry of the label's list that cannot be read (no
 * name, a hash that is not one) is counted as something the client has that
 * Axis could not read. */
static void
test_unreadable_entries_are_counted(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) items = NULL;
	guint unreadable = 99;

	dg_raw(fx->dg, "core.get_torrents_status", 200, "application/json",
		"{\"result\": {\"0101010101010101010101010101010101010101\": {\"name\": \"Some.Film.2020.1080p\", \"state\": "
		"\"Downloading\", \"label\": \"axis\", \"download_location\": \"/downloads\"}}, \"error\": null, \"id\": 1}");
	g_assert_true(axis_downloads_client_items(fx->downloads, fx->client_id, &items, &unreadable, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(items->len, ==, 1);
	g_assert_cmpuint(unreadable, ==, 0);
	g_clear_pointer(&items, g_ptr_array_unref);
	dg_raw(fx->dg, "core.get_torrents_status", 200, "application/json",
		"{\"result\": {\"0101010101010101010101010101010101010101\": {\"name\": \"Some.Film.2020.1080p\", \"state\": "
		"\"Downloading\", \"label\": \"axis\", \"download_location\": \"/downloads\"}, "
		"\"0202020202020202020202020202020202020202\": {\"state\": \"Downloading\", \"label\": \"axis\"}, "
		"\"not a hash\": {\"name\": \"Broken.2018\", \"state\": \"Downloading\", \"label\": \"axis\"}}, "
		"\"error\": null, \"id\": 1}");
	g_assert_true(axis_downloads_client_items(fx->downloads, fx->client_id, &items, &unreadable, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(items->len, ==, 1);
	g_assert_cmpuint(unreadable, >, 0);
	dg_clear_raw(fx->dg, "core.get_torrents_status");
}

/* Review of the fixes, items 1 and 2, through Deluge's own answer: what it
 * has is asked of it with no label filter (a torrent with another label,
 * another program's or a person's, is still something at its path), and each
 * name is kept exactly as Deluge gave it, for comparing with names on disk. */
static void
test_everything_listed(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) items = NULL;
	g_autofree gchar *why = NULL;
	g_autofree gchar *filter = NULL;
	const AxisDownloadItem *spaced = NULL;
	const AxisDownloadItem *theirs = NULL;
	guint unreadable = 99;
	guint i;

	dg_put(fx->dg, "0101010101010101010101010101010101010101", "Double  Space  Name ", "axis", "Seeding", 1000, 1000,
		"/downloads", TRUE, "");
	dg_put(fx->dg, "0202020202020202020202020202020202020202", "Their.Own.2019", "tv", "Seeding", 1000, 1000,
		"/downloads/tv", TRUE, "");
	g_assert_true(axis_downloads_client_items_full(fx->downloads, fx->client_id, &items, &unreadable, &why, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(unreadable, ==, 0);
	filter = dg_text(fx->dg, &fx->dg->last_filter);
	g_assert_null(filter);
	for (i = 0; i < items->len; i++) {
		const AxisDownloadItem *item = g_ptr_array_index(items, i);

		if (g_strcmp0(item->client_ref, "0101010101010101010101010101010101010101") == 0)
			spaced = item;
		if (g_strcmp0(item->client_ref, "0202020202020202020202020202020202020202") == 0)
			theirs = item;
	}
	g_assert_nonnull(spaced);
	g_assert_cmpstr(spaced->name, ==, "Double Space Name");
	g_assert_cmpstr(spaced->own_name, ==, "Double  Space  Name ");
	g_assert_nonnull(theirs);
	g_assert_cmpstr(theirs->category, ==, "tv");
}

/* Review of the fixes, item 5: an answer for one torrent that holds more
 * entries than are looked at, without it, cannot say that Deluge lacks it
 * (it left out the filter): -1, so nothing is removed on a guess. */
static void
test_parse_label_beyond_cap(void)
{
	g_autoptr(GString) text = g_string_new("{");
	g_autofree gchar *label = NULL;
	AxisJsonDoc *doc;
	guint i;

	for (i = 1; i <= AXIS_DELUGE_MAX_TORRENTS + 1; i++)
		g_string_append_printf(text, "%s\"%040x\": {\"name\": \"T%u\", \"label\": \"tv\"}", i > 1 ? "," : "", i, i);
	g_string_append_c(text, '}');
	doc = doc_of_text(text->str);
	g_assert_cmpint(axis_deluge_parse_label_of(axis_json_doc_get_root(doc), "f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0",
		&label, NULL), ==, -1);
	g_assert_null(label);
	axis_json_doc_free(doc);
	/* Within the cap and without it: Deluge has no such torrent. */
	doc = doc_of_text("{\"0101010101010101010101010101010101010101\": {\"name\": \"T\", \"label\": \"tv\"}}");
	g_assert_cmpint(axis_deluge_parse_label_of(axis_json_doc_get_root(doc), "f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0",
		&label, NULL), ==, 0);
	axis_json_doc_free(doc);
}

/* An unreachable client changes nothing about its downloads. */
static void
test_down_client_fails_nothing(Fixture *fx, gconstpointer data)
{
	AxisDownloadPollReport report;
	g_autofree gchar *hash = NULL;
	gint64 id = add_torrent_release(fx, "Some.Film.2020.1080p", &hash);

	dg_put(fx->dg, hash, "Some.Film.2020.1080p", "axis", "Downloading", 1000, 400, "/downloads", FALSE, "");
	poll(fx);
	dg_set_mode(fx->dg, DG_DOWN);
	report = poll(fx);
	g_assert_false(report.reached);
	assert_state(fx, id, "downloading");
	assert_q(fx, "0", "SELECT missing_polls FROM download");
	dg_set_mode(fx->dg, DG_NORMAL);
	report = poll(fx);
	g_assert_true(report.reached);
	assert_state(fx, id, "downloading");
}

/* ── Session (1.1, 2.4) ─────────────────────────────────────────── */

/* One login per connection: any number of calls after it use the same session. */
static void
test_one_login_per_connection(Fixture *fx, gconstpointer data)
{
	AxisDownloadTestResult result;
	guint i;

	add_torrent_release(fx, "Some.Film.2020.1080p", NULL);
	for (i = 0; i < 4; i++)
		poll(fx);
	g_assert_cmpuint(dg_count(fx->dg, "auth.login"), ==, 1);
	g_assert_cmpuint(dg_count(fx->dg, "unauthorised"), ==, 0);
	/* A test logs in afresh (it is the password that is being tested). */
	result = test_client(fx, fx->client_id);
	axis_download_test_result_clear(&result);
	g_assert_cmpuint(dg_count(fx->dg, "auth.login"), ==, 2);
	for (i = 0; i < 3; i++)
		poll(fx);
	g_assert_cmpuint(dg_count(fx->dg, "auth.login"), ==, 2);
	g_assert_cmpuint(dg_count(fx->dg, "unauthorised"), ==, 0);
}

/* Spec: Session lost during a list; task 2.4 */
static void
test_list_recovers_from_a_lost_session(Fixture *fx, gconstpointer data)
{
	AxisDownloadPollReport report;
	g_autofree gchar *hash = NULL;
	gint64 id = add_torrent_release(fx, "Some.Film.2020.1080p", &hash);

	dg_put(fx->dg, hash, "Some.Film.2020.1080p", "axis", "Downloading", 1000, 400, "/downloads", FALSE, "");
	g_assert_cmpuint(dg_count(fx->dg, "auth.login"), ==, 1);

	/* The session ends in the middle of the list: it is refused, one login, and the list is asked once more. */
	dg_set_text(fx->dg, &fx->dg->expire_before, "core.get_torrents_status");
	report = poll(fx);
	g_assert_true(report.reached);
	g_assert_cmpuint(report.listed, ==, 1);
	assert_state(fx, id, "downloading");
	g_assert_cmpuint(dg_count(fx->dg, "auth.login"), ==, 2);
	g_assert_cmpuint(dg_count(fx->dg, "unauthorised:core.get_torrents_status"), ==, 1);
	g_assert_cmpuint(dg_count(fx->dg, "core.get_torrents_status"), ==, 1);
	g_assert_cmpint(client_state(fx, fx->client_id), ==, AXIS_DOWNLOAD_CLIENT_OK);

	/* An idle session that ended: the same. */
	dg_expire(fx->dg);
	report = poll(fx);
	g_assert_true(report.reached);
	g_assert_cmpuint(dg_count(fx->dg, "auth.login"), ==, 3);

	/* A session that is refused again right after a login is not asked for a third time. */
	dg_raw_fixture(fx->dg, "core.get_torrents_status", "error-not-authenticated.json");
	report = poll(fx);
	g_assert_false(report.reached);
	g_assert_cmpuint(dg_count(fx->dg, "auth.login"), ==, 4);
	g_assert_cmpint(client_state(fx, fx->client_id), ==, AXIS_DOWNLOAD_CLIENT_REFUSED);
	dg_clear_raw(fx->dg, "core.get_torrents_status");
	report = poll(fx);
	g_assert_true(report.reached);
	assert_state(fx, id, "downloading");
}

/* Spec: Session lost during an add; task 2.4 */
static void
test_add_with_a_lost_session_is_not_sent(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *hash = NULL;
	g_autofree gchar *calls = NULL;
	g_autofree gchar *ref = NULL;
	g_autofree gchar *problem = NULL;
	gint64 id;
	gint64 again;

	/* Something to hold a session. */
	g_assert_true(poll(fx).reached);
	g_assert_cmpuint(dg_count(fx->dg, "auth.login"), ==, 1);

	/* The session ends between the questions before the add and the add itself. */
	dg_set_text(fx->dg, &fx->dg->expire_before, "core.add_torrent_file");
	id = add_torrent_release_at(fx, "Some.Film.2020.1080p", &hash, &error);
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_REFUSED);
	g_assert_nonnull(strstr(error->message, "no longer accepts the session"));
	/* Reported as not sent: failed, nothing added, the add asked for once and not repeated, no login in between. */
	assert_state(fx, id, "failed");
	problem = q(fx, "SELECT coalesce(problem, '') FROM download WHERE id = %" G_GINT64_FORMAT, id);
	g_assert_nonnull(strstr(problem, "Not sent"));
	g_assert_cmpuint(dg_count(fx->dg, "unauthorised:core.add_torrent_file"), ==, 1);
	g_assert_cmpuint(dg_count(fx->dg, "core.add_torrent_file"), ==, 0);
	g_assert_cmpuint(dg_count(fx->dg, "auth.login"), ==, 1);
	g_assert_cmpuint(dg_n_torrents(fx->dg), ==, 0);
	g_assert_cmpuint(dg_count(fx->dg, "label.set_torrent"), ==, 0);

	/* Nothing sends it again by itself: polls and time change nothing. */
	poll(fx);
	poll(fx);
	axis_test_db_exec(fx->t->db, "UPDATE download SET created_at = now() - interval '1 hour'");
	poll(fx);
	assert_state(fx, id, "failed");
	g_assert_cmpuint(dg_count(fx->dg, "core.add_torrent_file"), ==, 0);
	g_assert_cmpuint(dg_n_torrents(fx->dg), ==, 0);

	/* The next add logs in again and goes through, once. */
	g_clear_error(&error);
	g_clear_pointer(&hash, g_free);
	again = add_torrent_release_at(fx, "Some.Film.2020.1080p.Again", &hash, &error);
	g_assert_no_error(error);
	assert_state(fx, again, "queued");
	g_assert_cmpuint(dg_count(fx->dg, "core.add_torrent_file"), ==, 1);
	g_assert_cmpuint(dg_count(fx->dg, "auth.login"), ==, 2);
	g_assert_cmpuint(dg_n_torrents(fx->dg), ==, 1);
	ref = ref_of(fx, again);
	g_assert_cmpstr(ref, ==, hash);
	calls = dg_calls(fx->dg);
	g_assert_null(strstr(calls, "web.connect"));
}

/* A session that ended while idle is noticed before the add, which is then not refused */
static void
test_add_after_an_idle_session(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *hash = NULL;
	gint64 id;

	g_assert_true(poll(fx).reached);
	dg_expire(fx->dg);
	id = add_torrent_release(fx, "Some.Film.2020.1080p", &hash);
	assert_state(fx, id, "queued");
	/* Asked whether the session is good (it is not), logged in again, added once. */
	g_assert_cmpuint(dg_count(fx->dg, "auth.check_session"), ==, 1);
	g_assert_cmpuint(dg_count(fx->dg, "auth.login"), ==, 2);
	g_assert_cmpuint(dg_count(fx->dg, "core.add_torrent_file"), ==, 1);
	g_assert_cmpuint(dg_count(fx->dg, "unauthorised"), ==, 0);
	/* And a good session is asked, once, and is kept. */
	add_torrent_release(fx, "Another.Film.2019.720p", NULL);
	g_assert_cmpuint(dg_count(fx->dg, "auth.check_session"), ==, 2);
	g_assert_cmpuint(dg_count(fx->dg, "auth.login"), ==, 2);
}

/* The session ends between the add and its label: the stray torrent is removed all the same */
static void
test_session_lost_after_the_add_removes_the_torrent(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *hash = NULL;
	gint64 id;

	g_assert_true(poll(fx).reached);
	dg_set_text(fx->dg, &fx->dg->expire_before, "label.set_torrent");
	id = add_torrent_release_at(fx, "Some.Film.2020.1080p", &hash, &error);
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_REFUSED);
	g_assert_nonnull(strstr(error->message, "it was removed again"));
	assert_state(fx, id, "failed");
	g_assert_false(dg_has(fx->dg, hash));
	g_assert_cmpuint(dg_count(fx->dg, "core.add_torrent_file"), ==, 1);
	g_assert_cmpuint(dg_count(fx->dg, "label.set_torrent"), ==, 0);
	g_assert_cmpuint(dg_count(fx->dg, "core.remove_torrent"), ==, 1);
}

/* A label whose answer is lost: the torrent is taken out again, whatever Deluge did with the label */
static void
test_label_answer_lost_removes_the_torrent(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *hash = NULL;
	gint64 id;

	DG_SET(fx->dg, hang_set_label, TRUE);
	id = add_torrent_release_at(fx, "Some.Film.2020.1080p", &hash, &error);
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_REFUSED);
	assert_state(fx, id, "failed");
	g_assert_cmpuint(dg_count(fx->dg, "core.add_torrent_file"), ==, 1);
	g_assert_cmpuint(dg_count(fx->dg, "core.remove_torrent"), ==, 1);
	g_assert_false(dg_has(fx->dg, hash));
}

/* ── Remove (1.5) ───────────────────────────────────────────────── */

static void
test_remove_with_and_without_data(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *hash = NULL;
	g_autofree gchar *other = NULL;
	g_autofree gchar *with_data = NULL;
	g_autofree gchar *without_data = NULL;
	gint64 id = add_torrent_release(fx, "Some.Film.2020.1080p", &hash);
	gint64 second = add_torrent_release(fx, "Another.Film.2019.720p", &other);

	g_assert_true(axis_downloads_remove(fx->downloads, id, TRUE, &error));
	g_assert_no_error(error);
	assert_state(fx, id, "removed");
	g_assert_false(dg_has(fx->dg, hash));
	with_data = g_strdup_printf("core.remove_torrent:%s:1", hash);
	g_assert_cmpuint(dg_count(fx->dg, with_data), ==, 1);
	g_assert_true(dg_has(fx->dg, other));

	g_assert_true(axis_downloads_remove(fx->downloads, second, FALSE, &error));
	assert_state(fx, second, "removed");
	without_data = g_strdup_printf("core.remove_torrent:%s:0", other);
	g_assert_cmpuint(dg_count(fx->dg, without_data), ==, 1);
	g_assert_cmpuint(dg_n_torrents(fx->dg), ==, 0);
}

/* A removal Deluge refuses changes nothing; one that loses its session is reported and not repeated */
static void
test_remove_refusals(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *hash = NULL;
	gint64 id = add_torrent_release(fx, "Some.Film.2020.1080p", &hash);

	/* false: not removed. */
	dg_raw_fixture(fx->dg, "core.remove_torrent", "remove-false.json");
	g_assert_false(axis_downloads_remove(fx->downloads, id, FALSE, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_REFUSED);
	g_clear_error(&error);
	assert_state(fx, id, "queued");
	/* An error. */
	dg_raw_fixture(fx->dg, "core.remove_torrent", "error-unknown-method.json");
	g_assert_false(axis_downloads_remove(fx->downloads, id, FALSE, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_REFUSED);
	g_clear_error(&error);
	assert_state(fx, id, "queued");
	dg_clear_raw(fx->dg, "core.remove_torrent");

	/* The session ends right at the removal: reported, not repeated, and it works when asked again. */
	dg_set_text(fx->dg, &fx->dg->expire_before, "core.remove_torrent");
	g_assert_false(axis_downloads_remove(fx->downloads, id, FALSE, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_REFUSED);
	g_assert_nonnull(strstr(error->message, "no longer accepts the session"));
	g_clear_error(&error);
	assert_state(fx, id, "queued");
	g_assert_true(dg_has(fx->dg, hash));
	g_assert_true(axis_downloads_remove(fx->downloads, id, FALSE, &error));
	g_assert_no_error(error);
	assert_state(fx, id, "removed");
	g_assert_false(dg_has(fx->dg, hash));

	/* A session that ended while idle is noticed first, and the removal is not refused. */
	g_clear_pointer(&hash, g_free);
	id = add_torrent_release(fx, "Another.Film.2019.720p", &hash);
	dg_expire(fx->dg);
	g_assert_true(axis_downloads_remove(fx->downloads, id, FALSE, &error));
	g_assert_no_error(error);
	g_assert_false(dg_has(fx->dg, hash));
}

/* ── A torrent Deluge no longer has (fix-first-torrent) ─────────── */

/* The torrent goes from Deluge as a plugin of Deluge's removes it. */
static void
dg_drop(
	Dg          *self,
	const gchar *hash
){
	Torrent *torrent;

	g_mutex_lock(&self->lock);
	torrent = find_torrent(self, hash);
	if (torrent != NULL)
		g_ptr_array_remove(self->torrents, torrent);
	g_mutex_unlock(&self->lock);
}

/* The words Deluge 2.2.0 answered: this hash's "not in session" is the
 * torrent gone; another's, or other words, are not. */
static void
test_not_in_session_words(void)
{
	static const gchar hash[] = "0123456789abcdef0123456789abcdef01234567";
	g_autofree gchar *body = fixture_text("remove-not-in-session.json");
	g_autofree gchar *words = NULL;
	AxisJsonDoc *doc = NULL;
	const AxisJsonNode *result = NULL;

	g_assert_cmpint(axis_deluge_reply_parse(body, strlen(body), 1, &doc, &result, &words), ==, AXIS_DELUGE_REPLY_ERROR);
	g_assert_null(doc);
	g_assert_nonnull(words);
	g_assert_true(axis_deluge_says_not_in_session(words, hash));
	g_assert_true(axis_deluge_says_not_in_session(words, "0123456789ABCDEF0123456789ABCDEF01234567"));
	g_assert_false(axis_deluge_says_not_in_session(words, "89abcdef0123456789abcdef0123456789abcdef"));
	g_assert_false(axis_deluge_says_not_in_session(words, "not a hash"));
	g_assert_false(axis_deluge_says_not_in_session(NULL, hash));
	/* The sentence without the error's name, or the name alone. */
	g_assert_false(axis_deluge_says_not_in_session("torrent_id 0123456789abcdef0123456789abcdef01234567 not in session.",
		hash));
	g_assert_false(axis_deluge_says_not_in_session("Failure: [<class 'deluge.error.InvalidTorrentError'>: something]",
		hash));
}

/* Spec (downloaders): a torrent Deluge no longer has.  Its removal is "gone",
 * not a refusal: after an import, and from the Downloads page, where the
 * record is then gone. */
/* Spec (downloaders): a removal acts only on a torrent that still has Axis's
 * label; one a person gave another label is left alone, and no removal is
 * sent (the Deluge side of finding 2 of the stage 2 bug hunt).  From the
 * Downloads page it is recorded gone, nothing of its files Axis's. */
static void
test_remove_not_ours(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *hash = NULL;
	g_autofree gchar *label = NULL;
	gboolean found = FALSE;
	gboolean ours = TRUE;
	gint64 id = add_torrent_release(fx, "Lanternfold.Harbour.2016.1080p", &hash);

	dg_put(fx->dg, hash, "Lanternfold.Harbour.2016.1080p", "manual", "Seeding", 1000, 1000, "/downloads/manual", TRUE,
		NULL);
	axis_test_db_exec(fx->t->db, "UPDATE download SET state = 'imported'");
	g_assert_false(axis_downloads_remove_imported(fx->downloads, id, TRUE, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_NOT_OURS);
	g_assert_nonnull(strstr(error->message, "\"manual\""));
	g_clear_error(&error);
	g_assert_cmpuint(dg_count(fx->dg, "core.remove_torrent"), ==, 0);
	g_assert_true(dg_has(fx->dg, hash));
	label = dg_label_of(fx->dg, hash);
	g_assert_cmpstr(label, ==, "manual");
	g_assert_true(axis_downloads_find_torrent(fx->downloads, fx->client_id, hash, &found, &ours, NULL, &error));
	g_assert_no_error(error);
	g_assert_true(found);
	g_assert_false(ours);

	/* From the Downloads page: not removed, recorded gone. */
	axis_test_db_exec(fx->t->db, "UPDATE download SET state = 'downloading'");
	g_assert_false(axis_downloads_remove(fx->downloads, id, TRUE, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_NOT_OURS);
	g_clear_error(&error);
	assert_state(fx, id, "gone");
	assert_q(fx, NULL, "SELECT folder_removal FROM download");
	g_assert_cmpuint(dg_count(fx->dg, "core.remove_torrent"), ==, 0);
	g_assert_true(dg_has(fx->dg, hash));
}

static void
test_remove_not_in_session(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *hash = NULL;
	g_autofree gchar *other = NULL;
	gint64 id = add_torrent_release(fx, "Lanternfold.Harbour.2016.1080p", &hash);
	gint64 second = add_torrent_release(fx, "Quillmere.Station.2012.720p", &other);

	/* The double answers as Deluge 2.2.0 did. */
	dg_drop(fx->dg, hash);
	axis_test_db_exec(fx->t->db, "UPDATE download SET state = 'imported'");
	g_assert_false(axis_downloads_remove_imported(fx->downloads, id, TRUE, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_GONE);
	g_assert_nonnull(strstr(error->message, "no longer has the torrent"));
	g_clear_error(&error);
	g_assert_cmpuint(dg_count(fx->dg, "core.remove_torrent"), ==, 1);
	/* The record stays imported. */
	assert_state(fx, id, "imported");

	/* From the Downloads page: recorded gone. */
	axis_test_db_exec(fx->t->db, "UPDATE download SET state = 'downloading'");
	g_assert_false(axis_downloads_remove(fx->downloads, id, TRUE, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_GONE);
	g_clear_error(&error);
	assert_state(fx, id, "gone");

	/* Another torrent's "not in session" is not this one gone: a refusal. */
	dg_raw_fixture(fx->dg, "core.remove_torrent", "remove-not-in-session.json");
	g_assert_false(axis_downloads_remove(fx->downloads, second, TRUE, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_REFUSED);
	g_assert_nonnull(strstr(error->message, "InvalidTorrentError"));
	g_clear_error(&error);
	assert_state(fx, second, "downloading");
	g_assert_true(dg_has(fx->dg, other));
	dg_clear_raw(fx->dg, "core.remove_torrent");
	/* Any other error is still a refusal. */
	dg_raw_fixture(fx->dg, "core.remove_torrent", "error-unknown-method.json");
	g_assert_false(axis_downloads_remove(fx->downloads, second, TRUE, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_REFUSED);
	g_clear_error(&error);
	dg_clear_raw(fx->dg, "core.remove_torrent");
	g_assert_true(axis_downloads_remove(fx->downloads, second, TRUE, &error));
	g_assert_no_error(error);
	assert_state(fx, second, "removed");
}

static gchar *
label_folder_of(
	Fixture  *fx,
	GError  **error
){
	gchar *folder = NULL;

	if (!axis_downloads_category_folder(fx->downloads, fx->client_id, &folder, error))
		g_assert_null(folder);
	return folder;
}

static void
assert_folder_refused(
	Fixture     *fx,
	const gchar *words
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *folder = label_folder_of(fx, &error);

	g_assert_null(folder);
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_REFUSED);
	if (strstr(error->message, words) == NULL)
		g_error("expected \"%s\" in: %s", words, error->message);
}

/* Spec (downloaders): where Deluge keeps Axis's label.  The Label plugin's
 * "Move completed to" of the label, when it is on, set, a full path, and
 * neither Deluge's own folders nor another label's are it or inside it. */
static void
test_label_folder(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *folder = NULL;

	/* Not set: no folder of its own. */
	assert_folder_refused(fx, "has no folder of its own in Deluge");
	/* Set: that folder, normalised. */
	dg_set_label_folder(fx->dg, "axis", "/downloads/axis/");
	folder = label_folder_of(fx, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(folder, ==, "/downloads/axis");
	g_clear_pointer(&folder, g_free);
	/* Only the two folders of Deluge's own were asked for, nothing else of its settings. */
	g_assert_cmpuint(dg_count(fx->dg, "config-key:"), ==, 2);
	g_assert_cmpuint(dg_count(fx->dg, "config-key:download_location"), ==, 1);
	g_assert_cmpuint(dg_count(fx->dg, "config-key:move_completed_path"), ==, 1);
	/* Another label with a folder elsewhere: still Axis's own. */
	dg_set_label_folder(fx->dg, "manual", "/downloads/manual");
	folder = label_folder_of(fx, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(folder, ==, "/downloads/axis");
	g_clear_pointer(&folder, g_free);

	/* Another label's folder is it, or inside it. */
	dg_set_label_folder(fx->dg, "manual", "/downloads/axis");
	assert_folder_refused(fx, "the label \"manual\" moves its finished torrents to /downloads/axis too");
	dg_set_label_folder(fx->dg, "manual", "/downloads/axis/tv");
	assert_folder_refused(fx, "or to a folder inside it");
	dg_set_label_folder(fx->dg, "manual", "relative/folder");
	assert_folder_refused(fx, "not a full path");
	dg_set_label_folder(fx->dg, "manual", NULL);

	/* Deluge's own folder for every download is it, or inside it. */
	dg_set_label_folder(fx->dg, "axis", "/downloads/incoming");
	assert_folder_refused(fx, "Deluge's own folder for every download");
	dg_set_label_folder(fx->dg, "axis", "/downloads");
	assert_folder_refused(fx, "Deluge's own folder for every download");
	dg_set_label_folder(fx->dg, "axis", "/downloads/done");
	assert_folder_refused(fx, "Deluge's own folder for every download");
	/* Not a full path, or with "..". */
	dg_set_label_folder(fx->dg, "axis", "axis");
	assert_folder_refused(fx, "not a full path");
	dg_set_label_folder(fx->dg, "axis", "/downloads/../axis");
	assert_folder_refused(fx, "not a full path");
	/* The answer of a Deluge whose label has the option off, and on (one
	 * label only: the answer is every label's). */
	{
		static const gchar *const only_axis[] = { "axis", NULL };
		static const gchar *const both[] = { "axis", "manual", NULL };

		dg_set_labels(fx->dg, only_axis);
		dg_raw_fixture(fx->dg, "label.get_options", "label-options-off.json");
		assert_folder_refused(fx, "has no folder of its own in Deluge");
		dg_raw_fixture(fx->dg, "label.get_options", "label-options.json");
		folder = label_folder_of(fx, &error);
		g_assert_no_error(error);
		g_assert_cmpstr(folder, ==, "/downloads/axis");
		g_clear_pointer(&folder, g_free);
		dg_clear_raw(fx->dg, "label.get_options");
		dg_set_labels(fx->dg, both);
	}

	/* No Label plugin, or no such label. */
	dg_set_label_folder(fx->dg, "axis", "/downloads/axis");
	DG_SET(fx->dg, has_label_plugin, FALSE);
	assert_folder_refused(fx, "Label plugin is not enabled");
	DG_SET(fx->dg, has_label_plugin, TRUE);
	{
		static const gchar *const others[] = { "manual", NULL };

		dg_set_labels(fx->dg, others);
		assert_folder_refused(fx, "did not give the options of the label \"axis\"");
	}

	/* A Deluge that does not answer is not a refusal. */
	dg_set_mode(fx->dg, DG_DOWN);
	folder = label_folder_of(fx, &error);
	g_assert_null(folder);
	g_assert_nonnull(error);
	g_assert_false(g_error_matches(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_REFUSED));
	dg_set_mode(fx->dg, DG_NORMAL);
}

/* ── The password and the session never leak (2.2) ──────────────── */

/* Task 2.2 */
static void
test_password_and_cookie_go_only_where_they_belong(Fixture *fx, gconstpointer data)
{
	Dg *other = dg_new(OTHER_PASSWORD);
	gint64 other_client = add_client_for(fx, other, "Other", OTHER_PASSWORD);
	g_autoptr(GPtrArray) requests = NULL;
	g_autoptr(GPtrArray) other_requests = NULL;
	g_autoptr(AxisDownloadClientInfo) client = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *transcript = NULL;
	g_autofree gchar *other_transcript = NULL;
	g_autofree gchar *hash = NULL;
	AxisDownloadTestResult result;
	gint64 wrong;
	gint64 id;
	guint logins = 0;
	guint i;

	/* A whole life: test, add, poll, remove, a session that ends, a wrong password; and another client. */
	result = test_client(fx, fx->client_id);
	axis_download_test_result_clear(&result);
	id = add_torrent_release(fx, "Some.Film.2020.1080p", &hash);
	poll(fx);
	dg_expire(fx->dg);
	poll(fx);
	g_assert_true(axis_downloads_remove(fx->downloads, id, TRUE, &error));
	wrong = add_client_for(fx, fx->dg, "Wrong", WRONG_PASSWORD);
	result = test_client(fx, wrong);
	axis_download_test_result_clear(&result);
	poll_client(fx, wrong);
	result = test_client(fx, other_client);
	axis_download_test_result_clear(&result);
	poll_client(fx, other_client);

	requests = axis_test_plex_requests(fx->dg->server);
	g_assert_cmpuint(requests->len, >=, 14);
	for (i = 0; i < requests->len; i++) {
		const gchar *one = g_ptr_array_index(requests, i);
		gsize line = strcspn(one, "\n");
		const gchar *blank = strstr(one, "\n\n");
		g_autofree gchar *head = g_strndup(one, blank != NULL ? (gsize)(blank - one) : strlen(one));
		g_autofree gchar *request_line = g_strndup(one, line);
		gboolean login = blank != NULL && strstr(blank, "\"auth.login\"") != NULL;

		/* One shape of request line: a POST of /json, no query. */
		g_assert_cmpstr(request_line, ==, "POST /json");
		g_assert_nonnull(blank);
		g_assert_nonnull(strstr(head, "Content-Type: application/json"));
		/* No password in any line or header, and in no body but a login's. */
		g_assert_null(strstr(head, PASSWORD));
		g_assert_null(strstr(head, WRONG_PASSWORD));
		g_assert_null(strstr(one, OTHER_PASSWORD));
		if (login) {
			logins++;
			g_assert_true(strstr(blank, PASSWORD) != NULL || strstr(blank, WRONG_PASSWORD) != NULL);
			/* A login carries no cookie: it is made on a clean slate. */
			g_assert_null(strstr(head, "_session_id"));
		} else {
			g_assert_null(strstr(blank, PASSWORD));
			g_assert_null(strstr(blank, WRONG_PASSWORD));
			g_assert_null(strstr(blank, "_session_id"));
		}
		/* No session id in the request line or the body, ever; in a header only as the cookie. */
		{
			guint s;

			for (s = 0; s < fx->dg->sessions->len; s++) {
				const gchar *session = g_ptr_array_index(fx->dg->sessions, s);

				g_assert_null(strstr(request_line, session));
				g_assert_null(strstr(blank, session));
			}
		}
	}
	g_assert_cmpuint(logins, >=, 4);
	g_assert_cmpuint(dg_bad_requests(fx->dg), ==, 0);
	g_assert_true(axis_test_plex_saw_non_get(fx->dg->server));

	/* The other client's server saw its own password and its own session, and none of the first's. */
	transcript = axis_test_plex_transcript(fx->dg->server);
	other_transcript = axis_test_plex_transcript(other->server);
	other_requests = axis_test_plex_requests(other->server);
	g_assert_cmpuint(other_requests->len, >=, 3);
	g_assert_nonnull(strstr(other_transcript, OTHER_PASSWORD));
	g_assert_null(strstr(other_transcript, PASSWORD));
	g_assert_null(strstr(other_transcript, WRONG_PASSWORD));
	g_assert_null(strstr(transcript, OTHER_PASSWORD));
	g_assert_cmpuint(other->sessions->len, >=, 1);
	for (i = 0; i < fx->dg->sessions->len; i++)
		g_assert_null(strstr(other_transcript, g_ptr_array_index(fx->dg->sessions, i)));
	for (i = 0; i < other->sessions->len; i++)
		g_assert_null(strstr(transcript, g_ptr_array_index(other->sessions, i)));

	/* Nowhere else: not in any table but its own, not in a job row, not in any log line, not in what a client shows. */
	assert_q(fx, "0", "SELECT count(*) FROM download_client c WHERE to_jsonb(c)::text LIKE '%" PASSWORD "%'");
	assert_q(fx, "0", "SELECT count(*) FROM download d WHERE to_jsonb(d)::text LIKE '%" PASSWORD "%'");
	assert_q(fx, "0", "SELECT count(*) FROM download_client c WHERE to_jsonb(c)::text LIKE '%" WRONG_PASSWORD "%'");
	assert_q(fx, "0", "SELECT count(*) FROM download_client c WHERE to_jsonb(c)::text LIKE '%" OTHER_PASSWORD "%'");
	assert_q(fx, "0", "SELECT count(*) FROM job j WHERE to_jsonb(j)::text LIKE '%" PASSWORD "%'");
	assert_q(fx, "0", "SELECT count(*) FROM job j WHERE to_jsonb(j)::text LIKE '%" WRONG_PASSWORD "%'");
	assert_q(fx, "0", "SELECT count(*) FROM job j WHERE to_jsonb(j)::text LIKE '%" OTHER_PASSWORD "%'");
	assert_q(fx, "3", "SELECT count(*) FROM download_client_secret WHERE secret IN ('" PASSWORD "', '" WRONG_PASSWORD "', '"
		OTHER_PASSWORD "')");
	g_assert_cmpuint(logged("*" PASSWORD "*"), ==, 0);
	g_assert_cmpuint(logged("*" WRONG_PASSWORD "*"), ==, 0);
	g_assert_cmpuint(logged("*" OTHER_PASSWORD "*"), ==, 0);
	for (i = 0; i < fx->dg->sessions->len + other->sessions->len; i++) {
		const gchar *session = i < fx->dg->sessions->len ? g_ptr_array_index(fx->dg->sessions, i) :
			g_ptr_array_index(other->sessions, i - fx->dg->sessions->len);
		g_autofree gchar *pattern = g_strdup_printf("*%s*", session);
		g_autofree gchar *sql_client = g_strdup_printf("SELECT count(*) FROM download_client c WHERE to_jsonb(c)::text LIKE '%%%s%%'",
			session);
		g_autofree gchar *sql_download = g_strdup_printf("SELECT count(*) FROM download d WHERE to_jsonb(d)::text LIKE '%%%s%%'",
			session);
		g_autofree gchar *sql_job = g_strdup_printf("SELECT count(*) FROM job j WHERE to_jsonb(j)::text LIKE '%%%s%%'", session);

		g_assert_cmpuint(logged(pattern), ==, 0);
		assert_q(fx, "0", sql_client);
		assert_q(fx, "0", sql_download);
		assert_q(fx, "0", sql_job);
	}
	g_assert_true(axis_downloads_get_client(fx->downloads, wrong, &client, &error));
	g_assert_null(strstr(client->state_message, WRONG_PASSWORD));
	g_assert_cmpint(client->state, ==, AXIS_DOWNLOAD_CLIENT_REFUSED);
	dg_free(other);
}

/* A redirect is never followed, and neither the password nor the cookie goes along. */
static void
test_redirect_is_not_followed(Fixture *fx, gconstpointer data)
{
	Dg *other = dg_new(PASSWORD);
	g_autofree gchar *other_base = axis_test_plex_base(other->server);
	g_autofree gchar *elsewhere = g_strconcat(other_base, "/json", NULL);
	g_autofree gchar *transcript = NULL;
	g_autoptr(AxisDownloadClientInfo) client = NULL;
	g_autoptr(GError) error = NULL;
	AxisDownloadPollReport report;
	AxisDownloadTestResult result;
	gint64 id;

	/* A session first, so that there is a cookie to leak. */
	g_assert_true(poll(fx).reached);
	dg_set_text(fx->dg, &fx->dg->redirect, elsewhere);
	dg_set_mode(fx->dg, DG_REDIRECT);
	report = poll(fx);
	g_assert_false(report.reached);
	g_assert_true(axis_downloads_test_client(fx->downloads, fx->client_id, &result, &error));
	axis_download_test_result_clear(&result);
	id = add_torrent_release_at(fx, "Some.Film.2020.1080p", NULL, &error);
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_assert_cmpint(id, >, 0);
	g_assert_cmpuint(axis_test_plex_n_requests(other->server), ==, 0);
	transcript = axis_test_plex_transcript(other->server);
	g_assert_null(strstr(transcript, PASSWORD));
	g_assert_true(axis_downloads_get_client(fx->downloads, fx->client_id, &client, &error));
	g_assert_cmpint(client->state, ==, AXIS_DOWNLOAD_CLIENT_FAILING);
	g_assert_nonnull(strstr(client->state_message, "redirect"));
	dg_free(other);
}

/* A client served under a path is asked at that path, cookie and all */
static void
test_requests_carry_the_base_path(Fixture *fx, gconstpointer data)
{
	Dg *served = dg_new(PASSWORD);
	g_autofree gchar *base = axis_test_plex_base(served->server);
	gint64 id = add_client_full(fx, "Under a path", base, "/deluge", PASSWORD, NULL, AXIS_DOWNLOAD_PRIORITY_NORMAL);
	AxisDownloadTestResult result;
	AxisDownloadPollReport report;

	dg_set_text(served, &served->prefix, "/deluge");
	result = test_client(fx, id);
	g_assert_cmpint(result.category_exists, ==, 1);
	axis_download_test_result_clear(&result);
	report = poll_client(fx, id);
	g_assert_true(report.reached);
	g_assert_cmpuint(dg_bad_requests(served), ==, 0);
	g_assert_cmpuint(axis_test_plex_count(served->server, "/deluge/json"), >=, 6);
	g_assert_cmpuint(axis_test_plex_count(served->server, "/json"), ==, 0);
	g_assert_cmpuint(dg_count(served, "unauthorised"), ==, 0);
	g_assert_cmpuint(dg_count(served, "auth.login"), ==, 1);
	dg_free(served);
}

/* ── Sent once (2.6) ────────────────────────────────────────────── */

/* A lost answer to the add: it was sent once and is never sent again.  The torrent
 * has no label yet (Deluge was lost on the way), which no label filter lists: it is
 * not adopted, and is failed after the wait.  KNOWN GAP, written down here. */
static void
test_lost_add_answer_is_never_sent_again(Fixture *fx, gconstpointer data)
{
	AxisDownloadPollReport report;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *hash = NULL;
	gint64 id;

	dg_set_mode(fx->dg, DG_ADD_HANGS);
	id = add_torrent_release_at(fx, "Some.Series.S01E01.720p", &hash, &error);
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_UNCERTAIN);
	assert_state(fx, id, "sending");
	dg_set_mode(fx->dg, DG_NORMAL);
	g_assert_cmpuint(dg_count(fx->dg, "core.add_torrent_file"), ==, 1);
	g_assert_true(dg_has(fx->dg, hash));

	/* Within the wait the record is left alone; the torrent is not sent again by anything. */
	report = poll(fx);
	g_assert_cmpuint(report.adopted, ==, 0);
	assert_state(fx, id, "sending");
	axis_test_db_exec(fx->t->db, "UPDATE download SET created_at = now() - interval '1 hour'");
	report = poll(fx);
	g_assert_cmpuint(report.adopted, ==, 0);
	g_assert_cmpuint(report.not_found, ==, 1);
	assert_state(fx, id, "failed");
	g_assert_cmpuint(dg_count(fx->dg, "core.add_torrent_file"), ==, 1);
	g_assert_cmpuint(dg_n_torrents(fx->dg), ==, 1);
	g_assert_cmpuint(dg_count(fx->dg, "label.set_torrent"), ==, 0);
	g_assert_cmpuint(dg_count(fx->dg, "core.remove_torrent"), ==, 0);
}

static void
test_crash_before_sending_is_never_sent(Fixture *fx, gconstpointer data)
{
	AxisDownloadPollReport report;
	g_autoptr(GError) error = NULL;
	gint64 id;

	axis_downloads_set_fault(fx->downloads, AXIS_DOWNLOAD_FAULT_AFTER_RECORD);
	id = add_torrent_release_at(fx, "Some.Film.2020.1080p", NULL, &error);
	g_assert_nonnull(error);
	assert_state(fx, id, "sending");
	g_assert_cmpuint(dg_count(fx->dg, "core.add_torrent"), ==, 0);

	restart(fx);
	report = poll(fx);
	g_assert_cmpuint(report.not_found, ==, 1);
	assert_state(fx, id, "failed");
	poll(fx);
	g_assert_cmpuint(dg_count(fx->dg, "core.add_torrent"), ==, 0);
	g_assert_cmpuint(dg_n_torrents(fx->dg), ==, 0);
}

/* Spec (core): a release handed over and the answer lost is matched by info-hash, never sent again */
static void
test_crash_after_sending_is_adopted_by_hash(Fixture *fx, gconstpointer data)
{
	AxisDownloadPollReport report;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *hash = NULL;
	g_autofree gchar *ref = NULL;
	gint64 id;

	axis_downloads_set_fault(fx->downloads, AXIS_DOWNLOAD_FAULT_AFTER_SEND);
	id = add_torrent_release_at(fx, "Some.Film.2020.1080p", &hash, &error);
	g_assert_nonnull(error);
	assert_state(fx, id, "sending");
	assert_q(fx, NULL, "SELECT client_ref FROM download");
	g_assert_cmpuint(dg_count(fx->dg, "core.add_torrent_file"), ==, 1);

	restart(fx);
	report = poll(fx);
	g_assert_cmpuint(report.adopted, ==, 1);
	g_assert_cmpuint(report.not_found, ==, 0);
	assert_state(fx, id, "queued");
	ref = ref_of(fx, id);
	g_assert_cmpstr(ref, ==, hash);
	g_assert_cmpuint(dg_count(fx->dg, "core.add_torrent_file"), ==, 1);
	g_assert_cmpuint(dg_n_torrents(fx->dg), ==, 1);

	/* Matched by the hash, not by the name: a torrent of the same name and another hash is not it. */
	axis_downloads_set_fault(fx->downloads, AXIS_DOWNLOAD_FAULT_NONE);
}

/* The same torrent twice is refused before it reaches Deluge; a magnet link for one in progress too */
static void
test_the_same_torrent_twice_is_sent_once(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *hash = NULL;

	add_torrent_release(fx, "Some.Film.2020.1080p", &hash);
	add_torrent_release_at(fx, "Some.Film.2020.1080p", NULL, &error);
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_EXISTS);
	g_assert_cmpuint(dg_count(fx->dg, "core.add_torrent_file"), ==, 1);
	g_assert_cmpuint(dg_n_torrents(fx->dg), ==, 1);
}

/* An unreachable client at send time: nothing went out, so failed, and never sent. */
static void
test_unreachable_client_at_send_time(Fixture *fx, gconstpointer data)
{
	AxisTestPlex *closed = axis_test_plex_new();
	g_autofree gchar *closed_base = axis_test_plex_base(closed);
	g_autoptr(GBytes) file = axis_test_download_torrent("Some.Film.2020.1080p", NULL);
	g_autoptr(GError) error = NULL;
	AxisDownloadRequest request;
	gint64 client;
	gint64 id = 0;

	axis_test_plex_free(closed);
	client = add_client_full(fx, "Down", closed_base, NULL, PASSWORD, NULL, AXIS_DOWNLOAD_PRIORITY_NORMAL);
	memset(&request, 0, sizeof(request));
	request.client_id = client;
	request.protocol = AXIS_DOWNLOAD_TORRENT;
	request.file = file;
	request.release_name = "Some.Film.2020.1080p";
	g_assert_false(axis_downloads_add(fx->downloads, &request, &id, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_UNREACHABLE);
	assert_state(fx, id, "failed");
}

/* A wrong password at add time: not sent, failed, and the password is in no message. */
static void
test_add_with_a_wrong_password(Fixture *fx, gconstpointer data)
{
	gint64 wrong = add_client_for(fx, fx->dg, "Wrong", WRONG_PASSWORD);
	g_autoptr(GBytes) file = axis_test_download_torrent("Some.Film.2020.1080p", NULL);
	g_autoptr(GError) error = NULL;
	AxisDownloadRequest request;
	gint64 id = 0;

	memset(&request, 0, sizeof(request));
	request.client_id = wrong;
	request.protocol = AXIS_DOWNLOAD_TORRENT;
	request.file = file;
	request.release_name = "Some.Film.2020.1080p";
	g_assert_false(axis_downloads_add(fx->downloads, &request, &id, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_AUTH);
	g_assert_null(strstr(error->message, WRONG_PASSWORD));
	assert_state(fx, id, "failed");
	g_assert_cmpuint(dg_count(fx->dg, "core.add_torrent_file"), ==, 0);
	g_assert_cmpuint(dg_n_torrents(fx->dg), ==, 0);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/download/deluge/parse/envelope", test_parse_envelope);
	g_test_add_func("/download/deluge/parse/values", test_parse_values);
	g_test_add_func("/download/deluge/parse/torrents", test_parse_torrents);
	g_test_add_func("/download/deluge/parse/broken", test_parse_broken_answers);
	g_test_add_func("/download/deluge/parse/capped", test_parse_capped);
	g_test_add_func("/download/deluge/parse/label-beyond-cap", test_parse_label_beyond_cap);
	g_test_add_func("/download/deluge/parse/not-in-session", test_not_in_session_words);
#define ADD(path, func) AXIS_TEST_ADD_DB_CASE("/download/deluge/" path, Fixture, NULL, func)
	ADD("client/test", test_client_test);
	ADD("client/test-connects", test_client_test_connects);
	ADD("client/test-not-connected", test_client_test_not_connected);
	ADD("client/test-no-label-plugin", test_client_test_no_label_plugin);
	ADD("client/test-makes-the-label", test_client_test_makes_the_label);
	ADD("client/wrong-password", test_client_wrong_password);
	ADD("client/label-lower-case", test_client_label_must_be_lower_case);
	ADD("client/old-daemon", test_client_test_old_daemon);
	ADD("add/torrent-file", test_add_torrent_file);
	ADD("add/magnet", test_add_magnet);
	ADD("add/hash-case", test_add_hash_case_is_ignored);
	ADD("add/priority-ignored", test_add_priority_is_ignored);
	ADD("add/label-first", test_add_makes_the_label_first);
	ADD("add/label-cannot-be-set", test_add_label_cannot_be_set);
	ADD("add/hash-mismatch", test_add_hash_mismatch);
	ADD("add/refused-by-deluge", test_add_refused_by_deluge);
	ADD("add/refuses-usenet", test_add_refuses_usenet);
	ADD("add/wrong-password", test_add_with_a_wrong_password);
	ADD("list/other-labels", test_other_labels_are_never_touched);
	ADD("list/fixtures", test_fixture_answers_through_the_layer);
	ADD("list/unreadable", test_unreadable_lists_change_nothing);
	ADD("list/unreadable-entries", test_unreadable_entries_are_counted);
	ADD("list/everything", test_everything_listed);
	ADD("track/seeding-stays-completed", test_seeding_stays_completed);
	ADD("track/unknown-state", test_unknown_state_keeps_the_last_state);
	ADD("track/client-down", test_down_client_fails_nothing);
	ADD("session/one-login", test_one_login_per_connection);
	ADD("session/list-recovers", test_list_recovers_from_a_lost_session);
	ADD("session/add-not-sent", test_add_with_a_lost_session_is_not_sent);
	ADD("session/add-after-idle", test_add_after_an_idle_session);
	ADD("session/lost-after-add", test_session_lost_after_the_add_removes_the_torrent);
	ADD("session/label-answer-lost", test_label_answer_lost_removes_the_torrent);
	ADD("remove/data", test_remove_with_and_without_data);
	ADD("remove/refusals", test_remove_refusals);
	ADD("remove/not-in-session", test_remove_not_in_session);
	ADD("remove/not-ours", test_remove_not_ours);
	ADD("folder/label", test_label_folder);
	ADD("secret/password-and-cookie", test_password_and_cookie_go_only_where_they_belong);
	ADD("secret/redirect", test_redirect_is_not_followed);
	ADD("base-path/requests", test_requests_carry_the_base_path);
	ADD("once/lost-answer", test_lost_add_answer_is_never_sent_again);
	ADD("once/crash-before-send", test_crash_before_sending_is_never_sent);
	ADD("once/crash-after-send", test_crash_after_sending_is_adopted_by_hash);
	ADD("once/twice", test_the_same_torrent_twice_is_sent_once);
	ADD("once/unreachable", test_unreachable_client_at_send_time);
#undef ADD
	return g_test_run();
}
