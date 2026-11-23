/*
 * test-download-sabnzbd.c - the SABnzbd download client kind, against a fake
 * SABnzbd on loopback, a real database and a real volume in a scratch
 * directory
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * THE FIXTURES ARE HAND-WRITTEN.  No SABnzbd was available: the answers in
 * tests/fixtures/sabnzbd and the fake below follow SABnzbd's published API
 * format and the author's memory of its versions 3.x and 4.x, not a
 * recording.  What is believed but not verified is listed in
 * tests/fixtures/sabnzbd/README.md and in the report of the change.  These
 * tests show that the kind does what it means to against that format; they
 * do not show that the format is right.
 *
 * Spec scenarios (downloaders):
 *   SABnzbd client: Add; Failure inside a successful answer;
 *     Post-processing; Completed
 *   The SABnzbd key is never in an address
 *   A client's address may carry a base path: Served under a path;
 *     Unsafe prefix
 * and tasks 3.1 to 3.7 of add-downloader-sabnzbd.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "axis-test-case.h"
#include "axis-test-db.h"
#include "axis-test-download.h"
#include "axis-test-library.h"
#include "axis-test-plex.h"
#include "axis-test-storage.h"
#include "core/axis-http.h"
#include "download/axis-download.h"
#include "download/axis-download-private.h"
#include "download/axis-download-sabnzbd.h"
#include "download/axis-download-sabnzbd-parse.h"

#define KEY "sab-api-key-7f3a9c21d84e5b06"
#define WRONG_KEY "not-the-key-91c0de2a77b4"
#define FIXTURES "tests/fixtures/sabnzbd/"

#define NZB_TEXT "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n" \
	"<nzb xmlns=\"http://www.newzbin.com/DTD/2003/nzb\"><file poster=\"a@b.test\" date=\"1\" subject=\"x\">" \
	"<groups><group>alt.binaries.test</group></groups>" \
	"<segments><segment bytes=\"100\" number=\"1\">id1@x.test</segment></segments></file></nzb>\r\n"

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

static gint64
megabytes(gdouble mb)
{
	return (gint64)(mb * 1048576.0 + 0.5);
}

/* ── The fake SABnzbd ───────────────────────────────────────────── */

typedef struct
{
	gchar    *id;
	gchar    *name;
	gchar    *category;
	gchar    *status;
	gboolean  history;
	gdouble   mb;
	gdouble   mbleft;
	gchar    *storage;
	gchar    *fail_message;
} Job;

typedef struct
{
	guint   status;
	gchar  *type;
	gchar  *body;
} Raw;

typedef enum
{
	SAB_NORMAL,
	SAB_DOWN,		/* the head never comes */
	SAB_ADD_HANGS,		/* takes an added release, never answers */
	SAB_REDIRECT,		/* every request is answered with a redirect */
	SAB_HOSTNAME		/* every request is answered 403 by the host-name check (hostname-denied.txt) */
} SabMode;

typedef struct
{
	GHashTable *fields;	/* gchar * -> gchar * */
	GBytes     *file;
	gchar      *file_name;
	gchar      *file_type;
	gchar      *boundary;
	gboolean    multipart;
	gboolean    multipart_ok;
} Params;

typedef struct
{
	AxisTestPlex *server;
	GMutex        lock;
	gchar        *key;
	gchar        *prefix;	/* the path in front of /api that is served ("" for none) */
	SabMode       mode;
	gchar        *redirect;
	GPtrArray    *jobs;	/* (lock) Job */
	GPtrArray    *calls;	/* (lock) "mode" or "mode:delete:id:del=n" */
	GHashTable   *raw;	/* (lock) mode -> Raw */
	GPtrArray    *categories;	/* (lock) gchar * for get_cats */
	GPtrArray    *adds;	/* (lock) Params of every add */
	GHashTable   *last_query;	/* (lock) the fields of the last queue or history call */
	guint         next_id;
	guint         bad_requests;	/* not POST, a query string, the wrong path */
	/* What get_config answers: each category's folder setting (name -> dir;
	 * a category without one answers "dir": ""), and the folder for
	 * finished downloads (NULL: not given). */
	GHashTable   *category_dirs;	/* (lock) */
	gchar        *complete_dir;	/* (lock) */
	GHashTable   *last_config;	/* (lock) the fields of the last get_config call */
	/* Where SABnzbd's own paths are on the test's disk: a job's "storage"
	 * /a/b is @disk/a/b.  NULL: no files at all. */
	gchar        *disk;		/* (lock) */
} Sab;

static void
job_free(gpointer data)
{
	Job *job = data;

	g_free(job->id);
	g_free(job->name);
	g_free(job->category);
	g_free(job->status);
	g_free(job->storage);
	g_free(job->fail_message);
	g_free(job);
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
params_clear(Params *params)
{
	g_clear_pointer(&params->fields, g_hash_table_unref);
	g_clear_pointer(&params->file, g_bytes_unref);
	g_clear_pointer(&params->file_name, g_free);
	g_clear_pointer(&params->file_type, g_free);
	g_clear_pointer(&params->boundary, g_free);
}

static void
params_free(gpointer data)
{
	params_clear(data);
	g_free(data);
}

static const gchar *
param(
	const Params *params,
	const gchar  *name
){
	return params->fields != NULL ? g_hash_table_lookup(params->fields, name) : NULL;
}

static gssize
find_bytes(
	const guint8 *hay,
	gsize         hay_length,
	gsize         start,
	const gchar  *needle,
	gsize         needle_length
){
	gsize i;

	for (i = start; i + needle_length <= hay_length; i++) {
		if (hay[i] == (guint8)needle[0] && memcmp(hay + i, needle, needle_length) == 0)
			return (gssize)i;
	}
	return -1;
}

/* A header parameter of a part: name="value". */
static gchar *
disposition_value(
	const gchar *headers,
	const gchar *key
){
	g_autofree gchar *needle = g_strdup_printf("%s=\"", key);
	const gchar *at = strstr(headers, needle);
	const gchar *end;

	if (at == NULL)
		return NULL;
	at += strlen(needle);
	end = strchr(at, '"');
	return end != NULL ? g_strndup(at, (gsize)(end - at)) : NULL;
}

/* Strict: a delimiter at the very start, CRLF framing, a closing delimiter. */
static void
parse_multipart(
	GBytes      *body,
	const gchar *boundary,
	Params      *params
){
	gsize length = 0;
	const guint8 *data = g_bytes_get_data(body, &length);
	g_autofree gchar *delimiter = g_strdup_printf("--%s", boundary);
	g_autofree gchar *next_delimiter = g_strdup_printf("\r\n--%s", boundary);
	gsize dl = strlen(delimiter);
	gsize pos = 0;

	params->multipart = TRUE;
	params->boundary = g_strdup(boundary);
	if (length < dl || memcmp(data, delimiter, dl) != 0)
		return;
	while (TRUE) {
		gsize p = pos + dl;
		gssize header_end;
		gssize next;
		g_autofree gchar *headers = NULL;
		g_autofree gchar *name = NULL;
		g_autofree gchar *filename = NULL;

		if (p + 2 <= length && data[p] == '-' && data[p + 1] == '-') {
			params->multipart_ok = (p + 2 == length) || (p + 4 == length && data[p + 2] == '\r' && data[p + 3] == '\n');
			return;
		}
		if (p + 2 > length || data[p] != '\r' || data[p + 1] != '\n')
			return;
		header_end = find_bytes(data, length, p + 2, "\r\n\r\n", 4);
		if (header_end < 0)
			return;
		headers = g_strndup((const gchar *)data + p + 2, (gsize)header_end - (p + 2));
		next = find_bytes(data, length, (gsize)header_end + 4, next_delimiter, strlen(next_delimiter));
		if (next < 0)
			return;
		name = disposition_value(headers, "name");
		filename = disposition_value(headers, "filename");
		if (name == NULL)
			return;
		if (filename != NULL) {
			const gchar *type = strstr(headers, "Content-Type: ");

			params->file = g_bytes_new(data + header_end + 4, (gsize)next - ((gsize)header_end + 4));
			params->file_name = g_steal_pointer(&filename);
			params->file_type = type != NULL ? g_strndup(type + 14, strcspn(type + 14, "\r\n")) : NULL;
		} else {
			g_hash_table_insert(params->fields, g_steal_pointer(&name),
				g_strndup((const gchar *)data + header_end + 4, (gsize)next - ((gsize)header_end + 4)));
		}
		pos = (gsize)next + 2;
	}
}

static void
parse_form(
	GBytes *body,
	Params *params
){
	gsize length = 0;
	const gchar *data = g_bytes_get_data(body, &length);
	g_autofree gchar *text = g_strndup(data != NULL ? data : "", length);
	g_auto(GStrv) pairs = g_strsplit(text, "&", -1);
	guint i;

	for (i = 0; pairs[i] != NULL; i++) {
		gchar *eq = strchr(pairs[i], '=');

		if (eq == NULL)
			continue;
		*eq = '\0';
		g_hash_table_insert(params->fields, g_uri_unescape_string(pairs[i], NULL),
			g_uri_unescape_string(eq + 1, NULL));
	}
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
answer_json(
	AxisTestPlexAnswer *answer,
	const gchar        *text
){
	answer_text(answer, 200, "application/json", text);
}

static Job *
find_job(
	Sab         *self,
	const gchar *id
){
	guint i;

	for (i = 0; id != NULL && i < self->jobs->len; i++) {
		Job *job = g_ptr_array_index(self->jobs, i);

		if (strcmp(job->id, id) == 0)
			return job;
	}
	return NULL;
}

/* The queue's jobs from the @start'th, at most @limit of them (0: all). */
static gchar *
queue_json(
	Sab   *self,
	guint  start,
	guint  limit
){
	GString *out = g_string_new("{\"queue\": {\"version\": \"4.3.3\", \"status\": \"Downloading\", \"slots\": [");
	guint n = 0;
	guint shown = 0;
	guint i;

	for (i = 0; i < self->jobs->len; i++) {
		const Job *job = g_ptr_array_index(self->jobs, i);
		gint percent;

		if (job->history)
			continue;
		n++;
		if (n <= start || (limit > 0 && shown >= limit))
			continue;
		percent = job->mb > 0 ? (gint)((job->mb - job->mbleft) / job->mb * 100.0) : 0;
		g_string_append_printf(out, "%s{\"status\": \"%s\", \"index\": %u, \"mb\": \"%.2f\", \"mbleft\": \"%.2f\", "
			"\"filename\": \"%s\", \"cat\": \"%s\", \"timeleft\": \"%s\", \"percentage\": \"%d\", "
			"\"nzo_id\": \"%s\"}", shown > 0 ? ", " : "", job->status, n - 1, job->mb, job->mbleft, job->name,
			job->category, strcmp(job->status, "Downloading") == 0 ? "0:05:00" : "0:00:00", percent, job->id);
		shown++;
	}
	g_string_append_printf(out, "], \"noofslots\": %u}}", n);
	return g_string_free(out, FALSE);
}

/* The history's jobs from the @start'th, at most @limit of them (0: all). */
static gchar *
history_json(
	Sab   *self,
	guint  start,
	guint  limit
){
	GString *out = g_string_new("{\"history\": {\"slots\": [");
	guint n = 0;
	guint shown = 0;
	guint i;

	for (i = 0; i < self->jobs->len; i++) {
		const Job *job = g_ptr_array_index(self->jobs, i);

		if (!job->history)
			continue;
		n++;
		if (n <= start || (limit > 0 && shown >= limit))
			continue;
		g_string_append_printf(out, "%s{\"status\": \"%s\", \"category\": \"%s\", \"name\": \"%s\", "
			"\"nzb_name\": \"%s.nzb\", \"nzo_id\": \"%s\", \"bytes\": %" G_GINT64_FORMAT ", \"storage\": \"%s\", "
			"\"fail_message\": \"%s\"}", shown > 0 ? ", " : "", job->status, job->category, job->name, job->name,
			job->id, megabytes(job->mb), job->storage != NULL ? job->storage : "",
			job->fail_message != NULL ? job->fail_message : "");
		shown++;
	}
	g_string_append_printf(out, "], \"noofslots\": %u}}", n);
	return g_string_free(out, FALSE);
}

/* A folder of the test's disk, with what is in it (never through a link). */
static void
remove_disk_tree(const gchar *path)
{
	GDir *dir = NULL;
	const gchar *name;

	if (g_file_test(path, G_FILE_TEST_IS_DIR) && !g_file_test(path, G_FILE_TEST_IS_SYMLINK))
		dir = g_dir_open(path, 0, NULL);
	while (dir != NULL && (name = g_dir_read_name(dir)) != NULL) {
		g_autofree gchar *child = g_build_filename(path, name, NULL);

		remove_disk_tree(child);
	}
	if (dir != NULL)
		g_dir_close(dir);
	g_remove(path);
}

/* get_config's answer: the categories with their folders, or (section misc)
 * the folder for finished downloads, as SABnzbd 5.1.3 was seen to list the
 * categories ("config", "categories", objects with "name" and "dir"). */
static gchar *
config_json(
	Sab         *self,
	const Params *params
){
	const gchar *section = param(params, "section");
	GString *out;

	if (g_strcmp0(section, "misc") == 0) {
		if (self->complete_dir == NULL)
			return g_strdup("{\"config\": {}}");
		return g_strdup_printf("{\"config\": {\"misc\": {\"complete_dir\": \"%s\"}}}", self->complete_dir);
	}
	if (g_strcmp0(section, "categories") != 0)
		return g_strdup("{\"config\": {}}");
	out = g_string_new("{\"config\": {\"categories\": [{\"name\": \"*\", \"order\": 0, \"pp\": \"3\", \"script\": "
		"\"None\", \"dir\": \"\", \"priority\": 0}");
	{
		guint i;

		for (i = 0; i < self->categories->len; i++) {
			const gchar *name = g_ptr_array_index(self->categories, i);
			const gchar *dir = g_hash_table_lookup(self->category_dirs, name);

			g_string_append_printf(out, ", {\"name\": \"%s\", \"order\": %u, \"pp\": \"\", \"script\": \"Default\", "
				"\"dir\": \"%s\", \"newzbin\": \"\", \"priority\": -100}", name, i + 1, dir != NULL ? dir : "");
		}
	}
	g_string_append(out, "]}}");
	return g_string_free(out, FALSE);
}

static void
note(
	Sab         *self,
	const gchar *format,
	...
) G_GNUC_PRINTF(2, 3);

static void
note(
	Sab         *self,
	const gchar *format,
	...
){
	va_list ap;

	va_start(ap, format);
	g_ptr_array_add(self->calls, g_strdup_vprintf(format, ap));
	va_end(ap);
}

static void
sab_handle(
	const AxisTestPlexRequest *request,
	AxisTestPlexAnswer        *answer,
	gpointer                   user_data
){
	Sab *self = user_data;
	g_autofree gchar *want_path = g_strconcat(self->prefix, "/api", NULL);
	const gchar *content_type = axis_test_plex_request_header(request, "Content-Type");
	Params params;
	const gchar *mode;
	const gchar *key;
	Raw *raw;

	memset(&params, 0, sizeof(params));
	params.fields = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

	g_mutex_lock(&self->lock);
	if (self->mode == SAB_DOWN) {
		answer->hang = TRUE;
		goto out;
	}
	if (self->mode == SAB_REDIRECT) {
		answer->status = 307;
		answer->headers = g_new0(gchar *, 3);
		answer->headers[0] = g_strdup("Location");
		answer->headers[1] = g_strdup(self->redirect != NULL ? self->redirect : "/");
		goto out;
	}
	if (self->mode == SAB_HOSTNAME) {
		g_autofree gchar *denied = fixture_text("hostname-denied.txt");

		note(self, "hostname-denied");
		answer_text(answer, 403, "text/plain;charset=utf-8", denied);
		goto out;
	}
	if (strcmp(request->path, want_path) != 0) {
		self->bad_requests++;
		note(self, "wrong-path %s", request->path);
		answer_text(answer, 404, "text/plain", "not found");
		goto out;
	}
	if (strcmp(request->method, "POST") != 0 || request->query != NULL) {
		self->bad_requests++;
		note(self, "bad-request %s", request->method);
		answer_text(answer, 405, "text/plain", "POST only");
		goto out;
	}
	if (content_type != NULL && g_str_has_prefix(content_type, "multipart/form-data")) {
		const gchar *at = strstr(content_type, "boundary=");

		if (at != NULL)
			parse_multipart(request->body, at + 9, &params);
	} else {
		parse_form(request->body, &params);
	}
	mode = param(&params, "mode");
	if (mode == NULL) {
		answer_json(answer, "{\"status\": false, \"error\": \"No mode\"}");
		goto out;
	}

	/* The version needs no key; everything else does. */
	key = param(&params, "apikey");
	if (strcmp(mode, "version") != 0 && g_strcmp0(key, self->key) != 0) {
		note(self, "badkey %s", mode);
		answer_json(answer, "{\"status\": false, \"error\": \"API Key Incorrect\"}");
		goto out;
	}

	{
		const gchar *name = param(&params, "name");
		g_autofree gchar *what = NULL;

		if ((strcmp(mode, "queue") == 0 || strcmp(mode, "history") == 0) && g_strcmp0(name, "delete") == 0)
			what = g_strdup_printf("%s:delete:%s:del=%s", mode, param(&params, "value") != NULL ? param(&params, "value") : "",
				param(&params, "del_files") != NULL ? param(&params, "del_files") : "");
		else
			what = g_strdup(mode);
		note(self, "%s", what);
		raw = g_hash_table_lookup(self->raw, what);
		if (raw == NULL)
			raw = g_hash_table_lookup(self->raw, mode);
		if (raw != NULL && !(strcmp(mode, "addfile") == 0 && self->mode == SAB_ADD_HANGS)) {
			if (strcmp(mode, "queue") == 0 || strcmp(mode, "history") == 0) {
				g_hash_table_remove_all(self->last_query);
				{
					GHashTableIter iter;
					gpointer k;
					gpointer v;

					g_hash_table_iter_init(&iter, params.fields);
					while (g_hash_table_iter_next(&iter, &k, &v))
						g_hash_table_insert(self->last_query, g_strdup(k), g_strdup(v));
				}
			}
			answer_text(answer, raw->status, raw->type, raw->body);
			goto out;
		}
	}

	if (strcmp(mode, "version") == 0) {
		answer_json(answer, "{\"version\": \"4.3.3\"}");
	} else if (strcmp(mode, "get_cats") == 0) {
		GString *text = g_string_new("{\"categories\": [\"*\"");
		guint i;

		for (i = 0; i < self->categories->len; i++)
			g_string_append_printf(text, ", \"%s\"", (const gchar *)g_ptr_array_index(self->categories, i));
		g_string_append(text, "]}");
		answer_json(answer, text->str);
		g_string_free(text, TRUE);
	} else if (strcmp(mode, "addfile") == 0) {
		Job *job = g_new0(Job, 1);
		Params *kept = g_new0(Params, 1);
		g_autofree gchar *reply = NULL;
		const gchar *nzbname = param(&params, "nzbname");
		g_autofree gchar *from_file = NULL;

		if (params.file_name != NULL) {
			from_file = g_strdup(params.file_name);
			if (g_str_has_suffix(from_file, ".nzb"))
				from_file[strlen(from_file) - 4] = '\0';
		}
		job->id = g_strdup_printf("SABnzbd_nzo_t%04u", ++self->next_id);
		job->name = g_strdup(nzbname != NULL ? nzbname : from_file);
		job->category = g_strdup(param(&params, "cat") != NULL ? param(&params, "cat") : "*");
		job->status = g_strdup("Queued");
		job->mb = 1000.0;
		job->mbleft = 1000.0;
		g_ptr_array_add(self->jobs, job);
		/* Kept for the test to look at. */
		kept->fields = g_steal_pointer(&params.fields);
		kept->file = g_steal_pointer(&params.file);
		kept->file_name = g_steal_pointer(&params.file_name);
		kept->file_type = g_steal_pointer(&params.file_type);
		kept->boundary = g_steal_pointer(&params.boundary);
		kept->multipart = params.multipart;
		kept->multipart_ok = params.multipart_ok;
		g_ptr_array_add(self->adds, kept);
		if (self->mode == SAB_ADD_HANGS) {
			answer->hang = TRUE;
			goto out;
		}
		reply = g_strdup_printf("{\"status\": true, \"nzo_ids\": [\"%s\"]}", job->id);
		answer_json(answer, reply);
	} else if (strcmp(mode, "queue") == 0 || strcmp(mode, "history") == 0) {
		gboolean history = strcmp(mode, "history") == 0;
		g_autofree gchar *text = NULL;

		g_hash_table_remove_all(self->last_query);
		{
			GHashTableIter iter;
			gpointer k;
			gpointer v;

			g_hash_table_iter_init(&iter, params.fields);
			while (g_hash_table_iter_next(&iter, &k, &v))
				g_hash_table_insert(self->last_query, g_strdup(k), g_strdup(v));
		}
		if (g_strcmp0(param(&params, "name"), "delete") == 0) {
			Job *job = find_job(self, param(&params, "value"));
			g_autofree gchar *reply = NULL;

			if (job != NULL && job->history == history) {
				reply = g_strdup_printf(history ? "{\"status\": true}" : "{\"status\": true, \"nzo_ids\": [\"%s\"]}",
					job->id);
				/* As SABnzbd does (seen from 5.1.3): del_files=1 on the history
				 * deletes the files of a job that failed; a completed job's
				 * entry goes and its folder stays. */
				if (history && g_strcmp0(param(&params, "del_files"), "1") == 0 && self->disk != NULL &&
				    job->storage != NULL && job->storage[0] == '/' && strstr(job->storage, "..") == NULL &&
				    g_ascii_strcasecmp(job->status, "Failed") == 0) {
					g_autofree gchar *path = g_strconcat(self->disk, job->storage, NULL);
					g_autofree gchar *parent = g_path_get_dirname(path);
					g_autofree gchar *parent_name = g_path_get_basename(parent);

					note(self, "deleted-files:%s", job->id);
					/* A job reported by its one file: its job's folder goes. */
					if (g_file_test(path, G_FILE_TEST_IS_REGULAR) && !g_file_test(path, G_FILE_TEST_IS_SYMLINK) &&
					    g_strcmp0(parent_name, job->name) == 0)
						remove_disk_tree(parent);
					else
						remove_disk_tree(path);
				}
				g_ptr_array_remove(self->jobs, job);
			} else {
				reply = g_strdup(history ? "{\"status\": true}" : "{\"status\": true, \"nzo_ids\": []}");
			}
			answer_json(answer, reply);
			goto out;
		}
		/* The category the kind asks for is not applied: the layer's own
		 * filtering is what keeps other jobs out. */
		{
			/* As SABnzbd pages them: "start" and "limit" (0 or none: all). */
			guint start = param(&params, "start") != NULL ? (guint)g_ascii_strtoull(param(&params, "start"), NULL, 10) : 0;
			guint limit = param(&params, "limit") != NULL ? (guint)g_ascii_strtoull(param(&params, "limit"), NULL, 10) : 0;

			text = history ? history_json(self, start, limit) : queue_json(self, start, limit);
		}
		answer_json(answer, text);
	} else if (strcmp(mode, "get_config") == 0) {
		g_autofree gchar *text = config_json(self, &params);

		g_hash_table_remove_all(self->last_config);
		{
			GHashTableIter iter;
			gpointer k;
			gpointer v;

			g_hash_table_iter_init(&iter, params.fields);
			while (g_hash_table_iter_next(&iter, &k, &v))
				g_hash_table_insert(self->last_config, g_strdup(k), g_strdup(v));
		}
		answer_json(answer, text);
	} else {
		answer_json(answer, "{\"status\": false, \"error\": \"Unknown mode\"}");
	}
out:
	g_mutex_unlock(&self->lock);
	params_clear(&params);
}

static Sab *
sab_new(const gchar *key)
{
	Sab *self = g_new0(Sab, 1);

	g_mutex_init(&self->lock);
	self->key = g_strdup(key);
	self->prefix = g_strdup("");
	self->jobs = g_ptr_array_new_with_free_func(job_free);
	self->calls = g_ptr_array_new_with_free_func(g_free);
	self->raw = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, raw_free);
	self->categories = g_ptr_array_new_with_free_func(g_free);
	self->adds = g_ptr_array_new_with_free_func(params_free);
	self->last_query = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	self->last_config = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	self->category_dirs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	g_ptr_array_add(self->categories, g_strdup("axis"));
	g_ptr_array_add(self->categories, g_strdup("manual"));
	self->server = axis_test_plex_new();
	g_assert_nonnull(self->server);
	axis_test_plex_set_handler(self->server, sab_handle, self);
	return self;
}

static void
sab_free(Sab *self)
{
	if (self == NULL)
		return;
	axis_test_plex_free(self->server);
	g_ptr_array_unref(self->jobs);
	g_ptr_array_unref(self->calls);
	g_hash_table_unref(self->raw);
	g_ptr_array_unref(self->categories);
	g_ptr_array_unref(self->adds);
	g_hash_table_unref(self->last_query);
	g_hash_table_unref(self->last_config);
	g_hash_table_unref(self->category_dirs);
	g_free(self->complete_dir);
	g_free(self->disk);
	g_free(self->key);
	g_free(self->prefix);
	g_free(self->redirect);
	g_mutex_clear(&self->lock);
	g_free(self);
}

static void
sab_set_prefix(
	Sab         *self,
	const gchar *prefix
){
	g_mutex_lock(&self->lock);
	g_free(self->prefix);
	self->prefix = g_strdup(prefix);
	g_mutex_unlock(&self->lock);
}

static void
sab_set_mode(
	Sab     *self,
	SabMode  mode
){
	g_mutex_lock(&self->lock);
	self->mode = mode;
	g_mutex_unlock(&self->lock);
}

/* Every answer to @what ("queue", "history", "addfile", "queue:delete:ID:del=1")
 * is this, until it is cleared. */
static void
sab_raw(
	Sab         *self,
	const gchar *what,
	guint        status,
	const gchar *type,
	const gchar *body
){
	Raw *raw = g_new0(Raw, 1);

	raw->status = status;
	raw->type = g_strdup(type);
	raw->body = g_strdup(body);
	g_mutex_lock(&self->lock);
	g_hash_table_insert(self->raw, g_strdup(what), raw);
	g_mutex_unlock(&self->lock);
}

static void
sab_raw_fixture(
	Sab         *self,
	const gchar *what,
	const gchar *fixture
){
	g_autofree gchar *text = fixture_text(fixture);

	sab_raw(self, what, 200, "application/json", text);
}

static void
sab_clear_raw(
	Sab         *self,
	const gchar *what
){
	g_mutex_lock(&self->lock);
	g_hash_table_remove(self->raw, what);
	g_mutex_unlock(&self->lock);
}

static void
sab_put(
	Sab         *self,
	const gchar *id,
	const gchar *name,
	const gchar *category,
	const gchar *status,
	gboolean     history,
	gdouble      mb,
	gdouble      mbleft,
	const gchar *storage,
	const gchar *fail_message
){
	Job *job;

	g_mutex_lock(&self->lock);
	job = find_job(self, id);
	if (job == NULL) {
		job = g_new0(Job, 1);
		job->id = g_strdup(id);
		g_ptr_array_add(self->jobs, job);
	}
	g_free(job->name);
	job->name = g_strdup(name);
	g_free(job->category);
	job->category = g_strdup(category);
	g_free(job->status);
	job->status = g_strdup(status);
	job->history = history;
	job->mb = mb;
	job->mbleft = mbleft;
	g_free(job->storage);
	job->storage = g_strdup(storage);
	g_free(job->fail_message);
	job->fail_message = g_strdup(fail_message);
	g_mutex_unlock(&self->lock);
}

/* Moves a job to the history under @status. */
static void
sab_finish(
	Sab         *self,
	const gchar *id,
	const gchar *status,
	const gchar *storage,
	const gchar *fail_message
){
	Job *job;

	g_mutex_lock(&self->lock);
	job = find_job(self, id);
	g_assert_nonnull(job);
	job->history = TRUE;
	job->mbleft = 0;
	g_free(job->status);
	job->status = g_strdup(status);
	g_free(job->storage);
	job->storage = g_strdup(storage);
	g_free(job->fail_message);
	job->fail_message = g_strdup(fail_message);
	g_mutex_unlock(&self->lock);
}

static void
sab_status(
	Sab         *self,
	const gchar *id,
	const gchar *status,
	gdouble      mbleft
){
	Job *job;

	g_mutex_lock(&self->lock);
	job = find_job(self, id);
	g_assert_nonnull(job);
	g_free(job->status);
	job->status = g_strdup(status);
	job->mbleft = mbleft;
	g_mutex_unlock(&self->lock);
}

static gboolean
sab_has(
	Sab         *self,
	const gchar *id
){
	gboolean has;

	g_mutex_lock(&self->lock);
	has = find_job(self, id) != NULL;
	g_mutex_unlock(&self->lock);
	return has;
}

static void
sab_set_categories(
	Sab               *self,
	const gchar *const *names
){
	guint i;

	g_mutex_lock(&self->lock);
	g_ptr_array_set_size(self->categories, 0);
	for (i = 0; names[i] != NULL; i++)
		g_ptr_array_add(self->categories, g_strdup(names[i]));
	g_mutex_unlock(&self->lock);
}

/* Category @name's folder setting, as get_config gives it ("" for none). */
static void
sab_set_category_dir(
	Sab         *self,
	const gchar *name,
	const gchar *dir
){
	g_mutex_lock(&self->lock);
	g_hash_table_insert(self->category_dirs, g_strdup(name), g_strdup(dir));
	g_mutex_unlock(&self->lock);
}

static void
sab_set_complete_dir(
	Sab         *self,
	const gchar *dir
){
	g_mutex_lock(&self->lock);
	g_free(self->complete_dir);
	self->complete_dir = g_strdup(dir);
	g_mutex_unlock(&self->lock);
}

static void
sab_set_disk(
	Sab         *self,
	const gchar *disk
){
	g_mutex_lock(&self->lock);
	g_free(self->disk);
	self->disk = g_strdup(disk);
	g_mutex_unlock(&self->lock);
}

static gchar *
sab_last_config(
	Sab         *self,
	const gchar *field
){
	gchar *value;

	g_mutex_lock(&self->lock);
	value = g_strdup(g_hash_table_lookup(self->last_config, field));
	g_mutex_unlock(&self->lock);
	return value;
}

/* How many calls were exactly @what ("version", "addfile",
 * "queue:delete:ID:del=1"); with @prefix, those that begin with it. */
static guint
sab_count(
	Sab         *self,
	const gchar *what,
	gboolean     prefix
){
	guint n = 0;
	guint i;

	g_mutex_lock(&self->lock);
	for (i = 0; i < self->calls->len; i++) {
		const gchar *one = g_ptr_array_index(self->calls, i);

		if (prefix ? g_str_has_prefix(one, what) : strcmp(one, what) == 0)
			n++;
	}
	g_mutex_unlock(&self->lock);
	return n;
}

/* The calls in order, as one text. */
static gchar *
sab_calls(Sab *self)
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
sab_bad_requests(Sab *self)
{
	guint n;

	g_mutex_lock(&self->lock);
	n = self->bad_requests;
	g_mutex_unlock(&self->lock);
	return n;
}

/* The latest add's field (a copy), file or file name. */
static gchar *
sab_last_add(
	Sab         *self,
	const gchar *field
){
	gchar *value = NULL;

	g_mutex_lock(&self->lock);
	if (self->adds->len > 0) {
		const Params *last = g_ptr_array_index(self->adds, self->adds->len - 1);

		if (strcmp(field, "@file_name") == 0)
			value = g_strdup(last->file_name);
		else if (strcmp(field, "@file_type") == 0)
			value = g_strdup(last->file_type);
		else if (strcmp(field, "@boundary") == 0)
			value = g_strdup(last->boundary);
		else if (strcmp(field, "@multipart") == 0)
			value = g_strdup(last->multipart && last->multipart_ok ? "yes" : "no");
		else
			value = g_strdup(param(last, field));
	}
	g_mutex_unlock(&self->lock);
	return value;
}

static GBytes *
sab_last_file(Sab *self)
{
	GBytes *file = NULL;

	g_mutex_lock(&self->lock);
	if (self->adds->len > 0) {
		const Params *last = g_ptr_array_index(self->adds, self->adds->len - 1);

		file = last->file != NULL ? g_bytes_ref(last->file) : NULL;
	}
	g_mutex_unlock(&self->lock);
	return file;
}

static gchar *
sab_last_query(
	Sab         *self,
	const gchar *field
){
	gchar *value;

	g_mutex_lock(&self->lock);
	value = g_strdup(g_hash_table_lookup(self->last_query, field));
	g_mutex_unlock(&self->lock);
	return value;
}

/* ── The fixture ────────────────────────────────────────────────── */

typedef struct
{
	AxisTestLibrary *t;
	Sab             *sab;
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

	g_assert_true(axis_downloads_add_kind(downloads, axis_download_sabnzbd_kind(), &error));
	g_assert_no_error(error);
	memset(&tuning, 0, sizeof(tuning));
	tuning.request_timeout_ms = 1500;
	axis_downloads_set_tuning(downloads, &tuning);
	return downloads;
}

static gint64
add_client_at(
	Fixture     *fx,
	const gchar *name,
	const gchar *address,
	const gchar *base_path,
	const gchar *key,
	AxisDownloadPriority priority
){
	AxisDownloadClientSpec spec;
	g_autoptr(GError) error = NULL;
	gint64 id = 0;

	memset(&spec, 0, sizeof(spec));
	spec.name = name;
	spec.kind = AXIS_DOWNLOAD_SABNZBD_NAME;
	spec.address = address;
	spec.base_path = base_path;
	spec.priority = priority;
	spec.enabled = TRUE;
	spec.secret = key;
	g_assert_true(axis_downloads_save_client(fx->downloads, 0, &spec, &id, &error));
	g_assert_no_error(error);
	return id;
}

static gint64
add_client(
	Fixture     *fx,
	const gchar *name,
	const gchar *key
){
	g_autofree gchar *base = axis_test_plex_base(fx->sab->server);

	return add_client_at(fx, name, base, NULL, key, AXIS_DOWNLOAD_PRIORITY_HIGH);
}

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	memset(fx, 0, sizeof(*fx));
	axis_test_log_init();
	axis_test_log_reset();
	axis_download_sabnzbd_forget();
	fx->t = axis_test_library_new(4);
	if (fx->t == NULL)
		return;
	fx->disk = axis_test_library_disk(fx->t, "disk1");
	fx->volume_id = axis_test_library_add_volume(fx->t, fx->disk, "Disk one");
	fx->sab = sab_new(KEY);
	fx->downloads = layer_new(fx);
	fx->client_id = add_client(fx, "Usenet", KEY);
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	if (fx->t == NULL)
		return;
	axis_downloads_unref(fx->downloads);
	sab_free(fx->sab);
	g_free(fx->disk);
	axis_test_library_free(fx->t);
	axis_test_log_reset();
	axis_download_sabnzbd_forget();
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

static gint64
add_release_at(
	Fixture     *fx,
	const gchar *name,
	GError     **error
){
	g_autoptr(GBytes) nzb = g_bytes_new_static(NZB_TEXT, strlen(NZB_TEXT));
	AxisDownloadRequest request;
	gint64 id = 0;

	memset(&request, 0, sizeof(request));
	request.protocol = AXIS_DOWNLOAD_USENET;
	request.file = nzb;
	request.release_name = name;
	request.title_label = "Some Film (2020)";
	request.module = "movies";
	axis_downloads_add(fx->downloads, &request, &id, error);
	return id;
}

static gint64
add_release(
	Fixture     *fx,
	const gchar *name
){
	g_autoptr(GError) error = NULL;
	gint64 id = add_release_at(fx, name, &error);

	g_assert_no_error(error);
	g_assert_cmpint(id, >, 0);
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

/* A record of a download that SABnzbd (the fixture's answers) holds, as if
 * Axis had added it. */
static gint64
insert_record(
	Fixture     *fx,
	const gchar *ref,
	const gchar *name,
	const gchar *state
){
	g_autofree gchar *sql = g_strdup_printf("INSERT INTO download (client_id, client_name, client_ref, protocol, source,"
		" release_name, state) VALUES (%" G_GINT64_FORMAT ", 'Usenet', '%s', 'usenet', 'file', '%s', '%s')"
		" RETURNING id", fx->client_id, ref, name, state);
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
	axis_download_sabnzbd_forget();
	fx->downloads = layer_new(fx);
	axis_test_db_exec(fx->t->db, "UPDATE download SET created_at = now() - interval '1 hour' WHERE state = 'sending'");
}

/* ── Reading answers (no database) ──────────────────────────────── */

static AxisJsonDoc *
doc_of_text(const gchar *text)
{
	AxisJsonDoc *doc = axis_json_parse(text, strlen(text), NULL);

	g_assert_nonnull(doc);
	return doc;
}

static AxisJsonDoc *
doc_of(const gchar *fixture)
{
	g_autofree gchar *text = fixture_text(fixture);

	return doc_of_text(text);
}

static AxisSabnzbdReplyKind
reply_of_text(
	const gchar  *text,
	AxisJsonDoc **doc,
	gchar       **message
){
	return axis_sabnzbd_reply_parse(text, strlen(text), doc, message);
}

static AxisSabnzbdReplyKind
reply_of(
	const gchar  *fixture,
	AxisJsonDoc **doc,
	gchar       **message
){
	g_autofree gchar *text = fixture_text(fixture);

	return reply_of_text(text, doc, message);
}

static const AxisSabnzbdSlot *
slot_by_ref(
	GPtrArray   *slots,
	const gchar *ref
){
	guint i;

	for (i = 0; i < slots->len; i++) {
		const AxisSabnzbdSlot *slot = g_ptr_array_index(slots, i);

		if (strcmp(slot->item->client_ref, ref) == 0)
			return slot;
	}
	g_error("no slot %s", ref);
	return NULL;
}

/* Spec: Failure inside a successful answer (task 3.1) */
static void
test_parse_replies(void)
{
	AxisJsonDoc *doc = NULL;
	g_autofree gchar *message = NULL;
	g_autofree gchar *version = NULL;
	g_autofree gchar *id = NULL;
	g_autofree gchar *why = NULL;

	g_assert_cmpint(reply_of("version.json", &doc, &message), ==, AXIS_SABNZBD_REPLY_OK);
	g_assert_null(message);
	version = axis_sabnzbd_parse_version(axis_json_doc_get_root(doc));
	g_assert_cmpstr(version, ==, "4.3.3");
	axis_json_doc_free(doc);

	/* Refused inside a 200: its words, cleaned (no line break, no spaces twice). */
	g_assert_cmpint(reply_of("addfile-failed.json", &doc, &message), ==, AXIS_SABNZBD_REPLY_REFUSED);
	g_assert_null(doc);
	g_assert_cmpstr(message, ==, "Cannot read the file: it is not an NZB (<html>)");
	g_clear_pointer(&message, g_free);

	/* A wrong key is told apart from any other refusal. */
	g_assert_cmpint(reply_of("wrong-key.json", &doc, &message), ==, AXIS_SABNZBD_REPLY_BAD_KEY);
	g_assert_cmpstr(message, ==, "API Key Incorrect");
	g_clear_pointer(&message, g_free);
	g_assert_cmpint(reply_of_text("{\"status\": false, \"error\": \"API Key Required\"}", &doc, &message), ==,
		AXIS_SABNZBD_REPLY_BAD_KEY);
	g_clear_pointer(&message, g_free);
	g_assert_cmpint(reply_of("plain-error.txt", &doc, &message), ==, AXIS_SABNZBD_REPLY_BAD_KEY);
	g_clear_pointer(&message, g_free);
	g_assert_cmpint(reply_of_text("{\"status\": false}", &doc, &message), ==, AXIS_SABNZBD_REPLY_REFUSED);
	g_assert_null(message);

	/* Not one JSON object: unreadable, with a reason. */
	g_assert_cmpint(reply_of("not-an-object.json", &doc, &message), ==, AXIS_SABNZBD_REPLY_UNREADABLE);
	g_assert_nonnull(message);
	g_clear_pointer(&message, g_free);
	g_assert_cmpint(reply_of("truncated.json", &doc, &message), ==, AXIS_SABNZBD_REPLY_UNREADABLE);
	g_clear_pointer(&message, g_free);
	g_assert_cmpint(reply_of_text("", &doc, &message), ==, AXIS_SABNZBD_REPLY_UNREADABLE);
	g_clear_pointer(&message, g_free);
	g_assert_cmpint(reply_of_text("<html>Bad gateway</html>", &doc, &message), ==, AXIS_SABNZBD_REPLY_UNREADABLE);
	g_clear_pointer(&message, g_free);
	/* A status that is not a boolean is not a refusal (the queue has one of its own). */
	g_assert_cmpint(reply_of_text("{\"status\": \"Idle\"}", &doc, &message), ==, AXIS_SABNZBD_REPLY_OK);
	axis_json_doc_free(doc);
	g_assert_cmpint(reply_of_text("{\"status\": true}", &doc, &message), ==, AXIS_SABNZBD_REPLY_OK);
	axis_json_doc_free(doc);

	/* No version: none. */
	doc = doc_of_text("{\"nothing\": 1}");
	g_assert_null(axis_sabnzbd_parse_version(axis_json_doc_get_root(doc)));
	axis_json_doc_free(doc);

	/* Categories. */
	doc = doc_of("get-cats.json");
	g_assert_cmpint(axis_sabnzbd_parse_categories(axis_json_doc_get_root(doc), "axis"), ==, 1);
	g_assert_cmpint(axis_sabnzbd_parse_categories(axis_json_doc_get_root(doc), "AXIS"), ==, 1);
	g_assert_cmpint(axis_sabnzbd_parse_categories(axis_json_doc_get_root(doc), "other"), ==, 0);
	axis_json_doc_free(doc);
	doc = doc_of("get-cats-no-axis.json");
	g_assert_cmpint(axis_sabnzbd_parse_categories(axis_json_doc_get_root(doc), "axis"), ==, 0);
	axis_json_doc_free(doc);
	doc = doc_of_text("{\"categories\": \"all\"}");
	g_assert_cmpint(axis_sabnzbd_parse_categories(axis_json_doc_get_root(doc), "axis"), ==, -1);
	axis_json_doc_free(doc);

	/* An add: exactly one identifier, and status true. */
	doc = doc_of("addfile-ok.json");
	g_assert_true(axis_sabnzbd_parse_added(axis_json_doc_get_root(doc), &id, &why));
	g_assert_cmpstr(id, ==, "SABnzbd_nzo_k2m9ab");
	g_assert_null(why);
	axis_json_doc_free(doc);
	g_clear_pointer(&id, g_free);
	doc = doc_of("addfile-two-ids.json");
	g_assert_false(axis_sabnzbd_parse_added(axis_json_doc_get_root(doc), &id, &why));
	g_assert_null(id);
	g_assert_nonnull(strstr(why, "more than one"));
	axis_json_doc_free(doc);
	g_clear_pointer(&why, g_free);
	doc = doc_of("addfile-no-ids.json");
	g_assert_false(axis_sabnzbd_parse_added(axis_json_doc_get_root(doc), &id, &why));
	g_assert_nonnull(strstr(why, "no job"));
	axis_json_doc_free(doc);
	g_clear_pointer(&why, g_free);
	/* Without status true it is not a success, whatever else it says. */
	doc = doc_of_text("{\"nzo_ids\": [\"SABnzbd_nzo_k2m9ab\"]}");
	g_assert_false(axis_sabnzbd_parse_added(axis_json_doc_get_root(doc), &id, &why));
	axis_json_doc_free(doc);
	g_clear_pointer(&why, g_free);
	doc = doc_of_text("{\"status\": true, \"nzo_ids\": [\"has a space\"]}");
	g_assert_false(axis_sabnzbd_parse_added(axis_json_doc_get_root(doc), &id, &why));
	axis_json_doc_free(doc);
	g_clear_pointer(&why, g_free);
	doc = doc_of_text("{\"status\": false, \"error\": \"Out of disk space\"}");
	g_assert_false(axis_sabnzbd_parse_added(axis_json_doc_get_root(doc), &id, &why));
	g_assert_nonnull(strstr(why, "Out of disk space"));
	axis_json_doc_free(doc);
	g_clear_pointer(&why, g_free);

	/* A removal names what it removed. */
	doc = doc_of("removed-queue.json");
	g_assert_true(axis_sabnzbd_parse_removed(axis_json_doc_get_root(doc), "SABnzbd_nzo_dl0001"));
	g_assert_false(axis_sabnzbd_parse_removed(axis_json_doc_get_root(doc), "SABnzbd_nzo_other"));
	axis_json_doc_free(doc);
	doc = doc_of("removed-none.json");
	g_assert_false(axis_sabnzbd_parse_removed(axis_json_doc_get_root(doc), "SABnzbd_nzo_dl0001"));
	axis_json_doc_free(doc);
}

static void
assert_item(
	const AxisSabnzbdSlot *slot,
	AxisDownloadItemState  state,
	gint64                 size,
	gint64                 done,
	gint64                 eta
){
	g_assert_true(slot->known);
	g_assert_cmpint(slot->item->state, ==, state);
	g_assert_cmpint(slot->item->size_bytes, ==, size);
	g_assert_cmpint(slot->item->done_bytes, ==, done);
	g_assert_cmpint(slot->item->eta_s, ==, eta);
	g_assert_cmpint(slot->item->rate_bps, ==, -1);
}

/* Task 3.1, 2.3: the queue, with the state mapping, sizes and times */
static void
test_parse_queue(void)
{
	g_autoptr(AxisJsonDoc) doc = doc_of("queue.json");
	g_autoptr(GPtrArray) slots = axis_sabnzbd_parse_queue(axis_json_doc_get_root(doc));
	const AxisSabnzbdSlot *slot;

	g_assert_nonnull(slots);
	g_assert_cmpuint(slots->len, ==, 7);
	slot = slot_by_ref(slots, "SABnzbd_nzo_dl0001");
	assert_item(slot, AXIS_DOWNLOAD_ITEM_DOWNLOADING, megabytes(4120.55), megabytes(4120.55) - megabytes(1581.05), 316);
	g_assert_cmpstr(slot->item->name, ==, "Some.Film.2020.1080p.WEB-DL");
	g_assert_cmpstr(slot->item->category, ==, "axis");
	g_assert_cmpstr(slot->word, ==, "Downloading");
	g_assert_false(slot->history);
	g_assert_null(slot->item->path);
	/* Queued and Propagating: queued; nothing is done, and no time is given for what is not running. */
	assert_item(slot_by_ref(slots, "SABnzbd_nzo_q00002"), AXIS_DOWNLOAD_ITEM_QUEUED, megabytes(1500.0), 0, -1);
	assert_item(slot_by_ref(slots, "SABnzbd_nzo_pr0005"), AXIS_DOWNLOAD_ITEM_QUEUED, megabytes(50.0), 0, -1);
	assert_item(slot_by_ref(slots, "SABnzbd_nzo_pa0003"), AXIS_DOWNLOAD_ITEM_PAUSED, megabytes(700.25),
		megabytes(700.25) - megabytes(350.125), -1);
	/* Fetching and Grabbing: downloading (1:02:03 left is 3723 seconds). */
	assert_item(slot_by_ref(slots, "SABnzbd_nzo_fe0004"), AXIS_DOWNLOAD_ITEM_DOWNLOADING, megabytes(900.0),
		megabytes(900.0) - megabytes(100.0), 3723);
	assert_item(slot_by_ref(slots, "SABnzbd_nzo_gr0006"), AXIS_DOWNLOAD_ITEM_DOWNLOADING, 0, 0, 0);
	/* Another category is read like any other (the layer drops it). */
	g_assert_cmpstr(slot_by_ref(slots, "SABnzbd_nzo_ma0007")->item->category, ==, "manual");
}

/* Task 3.4, 2.3: the history, and post-processing as downloading at 100 % */
static void
test_parse_history(void)
{
	g_autoptr(AxisJsonDoc) doc = doc_of("history.json");
	g_autoptr(GPtrArray) slots = axis_sabnzbd_parse_history(axis_json_doc_get_root(doc));
	static const gchar *const after[] = {
		"SABnzbd_nzo_vf0001", "SABnzbd_nzo_rp0001", "SABnzbd_nzo_ex0001", "SABnzbd_nzo_mv0001", "SABnzbd_nzo_rn0001",
		"SABnzbd_nzo_qc0001", NULL
	};
	const AxisSabnzbdSlot *slot;
	guint i;

	g_assert_nonnull(slots);
	g_assert_cmpuint(slots->len, ==, 10);
	slot = slot_by_ref(slots, "SABnzbd_nzo_cp0001");
	assert_item(slot, AXIS_DOWNLOAD_ITEM_COMPLETED, 4320011264, 4320011264, -1);
	g_assert_cmpstr(slot->item->path, ==, "/downloads/complete/axis/Some.Film.2020.1080p.WEB-DL");
	g_assert_cmpstr(slot->item->name, ==, "Some.Film.2020.1080p.WEB-DL");
	g_assert_true(slot->history);
	g_assert_null(slot->item->message);

	slot = slot_by_ref(slots, "SABnzbd_nzo_fl0001");
	g_assert_true(slot->known);
	g_assert_cmpint(slot->item->state, ==, AXIS_DOWNLOAD_ITEM_FAILED);
	g_assert_cmpstr(slot->item->message, ==, "Aborted, cannot be completed - https://sabnzbd.org/not-complete");
	g_assert_null(slot->item->path);

	/* The statuses of the work after the download: downloading, all of it,
	 * and never a folder (whatever "storage" says: it is not final yet). */
	for (i = 0; after[i] != NULL; i++) {
		slot = slot_by_ref(slots, after[i]);
		assert_item(slot, AXIS_DOWNLOAD_ITEM_DOWNLOADING, 1073741824, 1073741824, -1);
		g_assert_null(slot->item->path);
	}
	/* Completed with no folder: completed, with nothing to say where. */
	slot = slot_by_ref(slots, "SABnzbd_nzo_cp0003");
	g_assert_cmpint(slot->item->state, ==, AXIS_DOWNLOAD_ITEM_COMPLETED);
	g_assert_null(slot->item->path);
	g_assert_cmpstr(slot_by_ref(slots, "SABnzbd_nzo_cp0002")->item->category, ==, "manual");
}

/* Task 3.1: unknown words, missing and wrong-typed fields, no list */
static void
test_parse_broken_answers(void)
{
	g_autoptr(AxisJsonDoc) doc = doc_of("queue-wrong-types.json");
	g_autoptr(GPtrArray) slots = axis_sabnzbd_parse_queue(axis_json_doc_get_root(doc));
	const AxisSabnzbdSlot *slot;
	AxisJsonDoc *other;
	GPtrArray *none;

	/* Entries without an identifier are skipped, an identifier is taken
	 * once: five are left, and the two whose identifier is read and not their
	 * name or their category, which say so (review of the fixes, item 7: the
	 * layer must know which job it is). */
	g_assert_nonnull(slots);
	g_assert_cmpuint(slots->len, ==, 7);
	g_assert_cmpstr(slot_by_ref(slots, "SABnzbd_nzo_ws0004")->item->unreadable, ==, "it has no name Axis can read");
	g_assert_cmpstr(slot_by_ref(slots, "SABnzbd_nzo_ws0007")->item->unreadable, ==, "its category cannot be read");
	g_assert_null(slot_by_ref(slots, "SABnzbd_nzo_ws0007")->item->category);
	g_assert_null(slot_by_ref(slots, "SABnzbd_nzo_ws0001")->item->unreadable);
	/* Garbage sizes and times: not known, not wrong. */
	assert_item(slot_by_ref(slots, "SABnzbd_nzo_ws0001"), AXIS_DOWNLOAD_ITEM_DOWNLOADING, -1, -1, -1);
	/* Out of range: no size, no left, no percentage; the time is fine. */
	assert_item(slot_by_ref(slots, "SABnzbd_nzo_ws0005"), AXIS_DOWNLOAD_ITEM_DOWNLOADING, -1, -1, 10);
	/* A status that is not a word is not a known one. */
	slot = slot_by_ref(slots, "SABnzbd_nzo_ws0006");
	g_assert_false(slot->known);
	g_assert_null(slot->word);
	/* More left than all: nothing done, never negative; two numbers of time. */
	assert_item(slot_by_ref(slots, "SABnzbd_nzo_ws0009"), AXIS_DOWNLOAD_ITEM_DOWNLOADING, megabytes(2000.0), 0, 62);
	/* No amount left: the percentage; four numbers of time are days to seconds. */
	assert_item(slot_by_ref(slots, "SABnzbd_nzo_ws0010"), AXIS_DOWNLOAD_ITEM_DOWNLOADING, megabytes(2000.0),
		megabytes(2000.0) / 4, 2 * 86400 + 3 * 3600 + 4 * 60 + 5);

	/* No list at all. */
	other = doc_of("queue-no-slots.json");
	g_assert_null(axis_sabnzbd_parse_queue(axis_json_doc_get_root(other)));
	g_assert_null(axis_sabnzbd_parse_history(axis_json_doc_get_root(other)));
	axis_json_doc_free(other);
	other = doc_of("history-no-slots.json");
	g_assert_null(axis_sabnzbd_parse_history(axis_json_doc_get_root(other)));
	axis_json_doc_free(other);
	other = doc_of_text("{\"queue\": [], \"history\": 4}");
	g_assert_null(axis_sabnzbd_parse_queue(axis_json_doc_get_root(other)));
	g_assert_null(axis_sabnzbd_parse_history(axis_json_doc_get_root(other)));
	axis_json_doc_free(other);

	/* Empty lists are lists. */
	other = doc_of("queue-empty.json");
	none = axis_sabnzbd_parse_queue(axis_json_doc_get_root(other));
	g_assert_nonnull(none);
	g_assert_cmpuint(none->len, ==, 0);
	g_ptr_array_unref(none);
	axis_json_doc_free(other);
	other = doc_of("history-empty.json");
	none = axis_sabnzbd_parse_history(axis_json_doc_get_root(other));
	g_assert_nonnull(none);
	g_assert_cmpuint(none->len, ==, 0);
	g_ptr_array_unref(none);
	axis_json_doc_free(other);

	/* Unknown words: the slot says so and holds the word. */
	other = doc_of("queue-unknown-status.json");
	none = axis_sabnzbd_parse_queue(axis_json_doc_get_root(other));
	g_assert_cmpuint(none->len, ==, 2);
	g_assert_false(slot_by_ref(none, "SABnzbd_nzo_dl0001")->known);
	g_assert_cmpstr(slot_by_ref(none, "SABnzbd_nzo_dl0001")->word, ==, "Wibbling");
	g_ptr_array_unref(none);
	axis_json_doc_free(other);
	other = doc_of("history-unknown-status.json");
	none = axis_sabnzbd_parse_history(axis_json_doc_get_root(other));
	g_assert_false(slot_by_ref(none, "SABnzbd_nzo_nw0002")->known);
	/* In the history an unknown word is still "downloading", all of it: never completed. */
	g_assert_cmpint(slot_by_ref(none, "SABnzbd_nzo_nw0002")->item->state, ==, AXIS_DOWNLOAD_ITEM_DOWNLOADING);
	g_ptr_array_unref(none);
	axis_json_doc_free(other);
}

/* Task 3.1: lists are capped; long strings are cut */
static void
test_parse_capped(void)
{
	GString *text = g_string_new("{\"queue\": {\"slots\": [");
	g_autofree gchar *long_name = g_strnfill(1000, 'n');
	AxisJsonDoc *doc;
	GPtrArray *slots;
	guint i;

	for (i = 0; i < 5200; i++)
		g_string_append_printf(text, "%s{\"status\": \"Queued\", \"mb\": \"1\", \"mbleft\": \"1\", \"filename\": \"Job %u\", "
			"\"cat\": \"axis\", \"nzo_id\": \"SABnzbd_nzo_c%u\"}", i > 0 ? "," : "", i, i);
	g_string_append(text, "]}}");
	doc = doc_of_text(text->str);
	slots = axis_sabnzbd_parse_queue(axis_json_doc_get_root(doc));
	g_assert_cmpuint(slots->len, ==, AXIS_SABNZBD_MAX_SLOTS);
	g_ptr_array_unref(slots);
	axis_json_doc_free(doc);
	g_string_free(text, TRUE);

	text = g_string_new(NULL);
	g_string_append_printf(text, "{\"queue\": {\"slots\": [{\"status\": \"Queued\", \"filename\": \"%s\", \"cat\": \"axis\","
		" \"nzo_id\": \"%s\"}, {\"status\": \"Queued\", \"filename\": \"x\", \"cat\": \"axis\", \"nzo_id\": \"SABnzbd_nzo_ok\"}]}}",
		long_name, long_name);
	doc = doc_of_text(text->str);
	slots = axis_sabnzbd_parse_queue(axis_json_doc_get_root(doc));
	/* An identifier that is too long is no identifier. */
	g_assert_cmpuint(slots->len, ==, 1);
	g_ptr_array_unref(slots);
	axis_json_doc_free(doc);
	g_string_free(text, TRUE);
	text = g_string_new(NULL);
	g_string_append_printf(text, "{\"queue\": {\"slots\": [{\"status\": \"Queued\", \"filename\": \"%s\", \"cat\": \"axis\","
		" \"nzo_id\": \"SABnzbd_nzo_ok\"}]}}", long_name);
	doc = doc_of_text(text->str);
	slots = axis_sabnzbd_parse_queue(axis_json_doc_get_root(doc));
	g_assert_cmpuint(g_utf8_strlen(((AxisSabnzbdSlot *)g_ptr_array_index(slots, 0))->item->name, -1), ==,
		AXIS_DOWNLOAD_NAME_MAX_CHARS);
	g_ptr_array_unref(slots);
	axis_json_doc_free(doc);
	g_string_free(text, TRUE);
}

static void
test_parse_figures(void)
{
	AxisJsonDoc *doc;
	gint64 bytes = 0;

	g_assert_cmpint(axis_sabnzbd_timeleft_parse("0:05:16"), ==, 316);
	g_assert_cmpint(axis_sabnzbd_timeleft_parse("1:02:03:04"), ==, 86400 + 2 * 3600 + 3 * 60 + 4);
	g_assert_cmpint(axis_sabnzbd_timeleft_parse("05:16"), ==, 316);
	g_assert_cmpint(axis_sabnzbd_timeleft_parse("316"), ==, -1);
	g_assert_cmpint(axis_sabnzbd_timeleft_parse("unknown"), ==, -1);
	g_assert_cmpint(axis_sabnzbd_timeleft_parse("1:2:3:4:5"), ==, -1);
	g_assert_cmpint(axis_sabnzbd_timeleft_parse("-1:00:00"), ==, -1);
	g_assert_cmpint(axis_sabnzbd_timeleft_parse("0:0x:00"), ==, -1);
	g_assert_cmpint(axis_sabnzbd_timeleft_parse(""), ==, -1);
	g_assert_cmpint(axis_sabnzbd_timeleft_parse(NULL), ==, -1);
	g_assert_cmpint(axis_sabnzbd_timeleft_parse("9999999:00:00"), ==, -1);

	doc = doc_of_text("\"1949.06\"");
	g_assert_true(axis_sabnzbd_megabytes_parse(axis_json_doc_get_root(doc), &bytes));
	g_assert_cmpint(bytes, ==, megabytes(1949.06));
	axis_json_doc_free(doc);
	doc = doc_of_text("12.5");
	g_assert_true(axis_sabnzbd_megabytes_parse(axis_json_doc_get_root(doc), &bytes));
	g_assert_cmpint(bytes, ==, 13107200);
	axis_json_doc_free(doc);
	doc = doc_of_text("\"0\"");
	g_assert_true(axis_sabnzbd_megabytes_parse(axis_json_doc_get_root(doc), &bytes));
	g_assert_cmpint(bytes, ==, 0);
	axis_json_doc_free(doc);
	{
		static const gchar *const bad[] = {
			"\"-1\"", "\"1e999\"", "\"abc\"", "\"\"", "null", "true", "[1]", "\"2000000000\"", "\"NaN\"", "\"0x10\"", NULL
		};
		guint i;

		for (i = 0; bad[i] != NULL; i++) {
			doc = doc_of_text(bad[i]);
			g_assert_false(axis_sabnzbd_megabytes_parse(axis_json_doc_get_root(doc), &bytes));
			axis_json_doc_free(doc);
		}
	}
	g_assert_true(axis_sabnzbd_ref_valid("SABnzbd_nzo_k2m9ab"));
	g_assert_false(axis_sabnzbd_ref_valid(""));
	g_assert_false(axis_sabnzbd_ref_valid("a b"));
	g_assert_false(axis_sabnzbd_ref_valid("a/b"));
	g_assert_false(axis_sabnzbd_ref_valid(NULL));
}

/* Task 3.5: accepted and refused forms of a base path */
static void
test_base_path_rules(void)
{
	static const gchar *const accepted[] = {
		"", "  ", "/", "//", "/sabnzbd", "/sabnzbd/", "/a/b/c", "/Sab-nzbd_1.2~x", "/...", " /sab ", "/a.b/c..d", NULL
	};
	static const gchar *const stored[] = {
		"", "", "", "", "/sabnzbd", "/sabnzbd", "/a/b/c", "/Sab-nzbd_1.2~x", "/...", "/sab", "/a.b/c..d", NULL
	};
	static const gchar *const refused[] = {
		"sabnzbd", "sabnzbd/", "/../etc", "/a/../b", "/a/..", "/..", "/.", "/a/./b", "/a//b", "/a?x=1", "/a#frag",
		"/a b", "/a%2e%2e", "/a%2Fb", "/a\\b", "/a\nb", "/a:80", "/a@b", "/é", "http://evil.test/", "//evil.test/x",
		"/a;b", "/a,b", "/a=b", "/a&b", "/a+b", "/a*b", "/a'b", "/a\"b", "/a<b", "/a|b", NULL
	};
	g_autofree gchar *long_path = g_strnfill(AXIS_DOWNLOAD_BASE_PATH_MAX + 1, 'x');
	g_autofree gchar *just_fits = NULL;
	g_autofree gchar *too_long = g_strconcat("/", long_path, NULL);
	g_autofree gchar *fits = NULL;
	guint i;

	for (i = 0; accepted[i] != NULL; i++) {
		g_autofree gchar *normalized = NULL;
		const gchar *problem = NULL;

		g_assert_true(axis_download_base_path_normalize(accepted[i], &normalized, &problem));
		g_assert_null(problem);
		g_assert_cmpstr(normalized, ==, stored[i]);
	}
	g_assert_true(axis_download_base_path_normalize(NULL, NULL, NULL));
	for (i = 0; refused[i] != NULL; i++) {
		g_autofree gchar *normalized = NULL;
		const gchar *problem = NULL;

		g_assert_false(axis_download_base_path_normalize(refused[i], &normalized, &problem));
		g_assert_null(normalized);
		g_assert_nonnull(problem);
		/* The reason never holds what was typed (past a few characters). */
		if (strlen(refused[i]) > 3)
			g_assert_null(strstr(problem, refused[i] + 1));
	}
	fits = g_strnfill(AXIS_DOWNLOAD_BASE_PATH_MAX - 1, 'x');
	just_fits = g_strconcat("/", fits, NULL);
	g_assert_cmpuint(strlen(just_fits), ==, AXIS_DOWNLOAD_BASE_PATH_MAX);
	g_assert_true(axis_download_base_path_normalize(just_fits, NULL, NULL));
	g_assert_false(axis_download_base_path_normalize(too_long, NULL, NULL));
}

/* ── Test (2.1) ─────────────────────────────────────────────────── */

static void
test_client_test(Fixture *fx, gconstpointer data)
{
	AxisDownloadTestResult result;
	g_autoptr(AxisDownloadClientInfo) client = NULL;
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_downloads_test_client(fx->downloads, fx->client_id, &result, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(result.version, ==, "4.3.3");
	g_assert_cmpint(result.category_exists, ==, 1);
	axis_download_test_result_clear(&result);
	g_assert_true(axis_downloads_get_client(fx->downloads, fx->client_id, &client, &error));
	g_assert_cmpint(client->state, ==, AXIS_DOWNLOAD_CLIENT_OK);
	g_assert_cmpstr(client->version, ==, "4.3.3");
	g_assert_cmpint(client->category_exists, ==, 1);
	g_assert_cmpint(client->last_contact_us, >, 0);
	/* The version, then the categories; nothing else. */
	g_assert_cmpuint(sab_count(fx->sab, "version", FALSE), ==, 1);
	g_assert_cmpuint(sab_count(fx->sab, "get_cats", FALSE), ==, 1);
	g_assert_cmpuint(axis_test_plex_n_requests(fx->sab->server), ==, 2);
}

static void
test_client_test_category_missing(Fixture *fx, gconstpointer data)
{
	static const gchar *const names[] = { "movies", "tv", NULL };
	AxisDownloadTestResult result;
	g_autoptr(AxisDownloadClientInfo) client = NULL;
	g_autoptr(GError) error = NULL;

	sab_set_categories(fx->sab, names);
	g_assert_true(axis_downloads_test_client(fx->downloads, fx->client_id, &result, &error));
	g_assert_no_error(error);
	g_assert_cmpint(result.category_exists, ==, 0);
	g_assert_nonnull(strstr(result.note, "no category called \"axis\""));
	axis_download_test_result_clear(&result);
	g_assert_true(axis_downloads_get_client(fx->downloads, fx->client_id, &client, &error));
	g_assert_cmpint(client->state, ==, AXIS_DOWNLOAD_CLIENT_OK);
	g_assert_cmpint(client->category_exists, ==, 0);
}

/* Task 2.1: a wrong key is told apart from an unreachable client */
static void
test_client_wrong_key_is_not_unreachable(Fixture *fx, gconstpointer data)
{
	gint64 wrong = add_client(fx, "Wrong", WRONG_KEY);
	AxisTestPlex *closed = axis_test_plex_new();
	g_autofree gchar *closed_base = axis_test_plex_base(closed);
	g_autoptr(AxisDownloadClientInfo) client = NULL;
	g_autoptr(GError) error = NULL;
	AxisDownloadTestResult result;
	gint64 down;

	/* SABnzbd answers the version without a key and the categories with
	 * "API Key Incorrect": the second call tells. */
	g_assert_true(axis_downloads_test_client(fx->downloads, wrong, &result, &error));
	g_assert_no_error(error);
	g_assert_nonnull(strstr(result.note, "refused the API key"));
	g_assert_nonnull(strstr(result.note, "API Key Incorrect"));
	g_assert_null(strstr(result.note, WRONG_KEY));
	axis_download_test_result_clear(&result);
	g_assert_true(axis_downloads_get_client(fx->downloads, wrong, &client, &error));
	g_assert_cmpint(client->state, ==, AXIS_DOWNLOAD_CLIENT_REFUSED);
	g_assert_null(strstr(client->state_message, WRONG_KEY));
	g_assert_cmpuint(sab_count(fx->sab, "badkey get_cats", FALSE), ==, 1);
	g_clear_pointer(&client, axis_download_client_info_free);

	/* Nothing listening: unreachable, not refused. */
	axis_test_plex_free(closed);
	down = add_client_at(fx, "Down", closed_base, NULL, KEY, AXIS_DOWNLOAD_PRIORITY_NORMAL);
	g_assert_true(axis_downloads_test_client(fx->downloads, down, &result, &error));
	axis_download_test_result_clear(&result);
	g_assert_true(axis_downloads_get_client(fx->downloads, down, &client, &error));
	g_assert_cmpint(client->state, ==, AXIS_DOWNLOAD_CLIENT_UNREACHABLE);
	g_clear_pointer(&client, axis_download_client_info_free);

	/* Something that answers but is not SABnzbd: failing, not refused. */
	sab_raw(fx->sab, "version", 200, "text/html", "<html><body>Welcome to nginx</body></html>");
	g_assert_true(axis_downloads_test_client(fx->downloads, fx->client_id, &result, &error));
	axis_download_test_result_clear(&result);
	g_assert_true(axis_downloads_get_client(fx->downloads, fx->client_id, &client, &error));
	g_assert_cmpint(client->state, ==, AXIS_DOWNLOAD_CLIENT_FAILING);
	g_clear_pointer(&client, axis_download_client_info_free);

	/* An answer with no version in it. */
	sab_raw(fx->sab, "version", 200, "application/json", "{\"hello\": 1}");
	g_assert_true(axis_downloads_test_client(fx->downloads, fx->client_id, &result, &error));
	axis_download_test_result_clear(&result);
	g_assert_true(axis_downloads_get_client(fx->downloads, fx->client_id, &client, &error));
	g_assert_cmpint(client->state, ==, AXIS_DOWNLOAD_CLIENT_FAILING);
	g_assert_cmpuint(logged("*" WRONG_KEY "*"), ==, 0);
}

/* Task 2.1: the old SABnzbd's plain-text refusal of a key */
static void
test_client_old_plain_text_wrong_key(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisDownloadClientInfo) client = NULL;
	g_autoptr(GError) error = NULL;
	AxisDownloadTestResult result;
	g_autofree gchar *text = fixture_text("plain-error.txt");

	sab_raw(fx->sab, "get_cats", 200, "text/plain", text);
	g_assert_true(axis_downloads_test_client(fx->downloads, fx->client_id, &result, &error));
	axis_download_test_result_clear(&result);
	g_assert_true(axis_downloads_get_client(fx->downloads, fx->client_id, &client, &error));
	g_assert_cmpint(client->state, ==, AXIS_DOWNLOAD_CLIENT_REFUSED);
}

/* Verified against SABnzbd 5.1.3: a host name outside host_whitelist is
 * answered 403 with plain text, and that is not a refused key. */
static void
test_client_hostname_check_is_not_a_refused_key(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisDownloadClientInfo) client = NULL;
	g_autoptr(GError) error = NULL;
	AxisDownloadTestResult result;

	sab_set_mode(fx->sab, SAB_HOSTNAME);
	g_assert_true(axis_downloads_test_client(fx->downloads, fx->client_id, &result, &error));
	g_assert_no_error(error);
	g_assert_nonnull(result.note);
	g_assert_nonnull(strstr(result.note, "SABnzbd refused the host name Axis used for it (127.0.0.1)"));
	g_assert_nonnull(strstr(result.note, "host_whitelist"));
	g_assert_null(strstr(result.note, "The API key or password was refused"));
	g_assert_null(strstr(result.note, "api key"));
	g_assert_null(strstr(result.note, "API key"));
	g_assert_null(strstr(result.note, "Hostname verification failed"));
	axis_download_test_result_clear(&result);
	g_assert_true(axis_downloads_get_client(fx->downloads, fx->client_id, &client, &error));
	g_assert_cmpint(client->state, !=, AXIS_DOWNLOAD_CLIENT_REFUSED);
	g_assert_cmpint(client->state, ==, AXIS_DOWNLOAD_CLIENT_FAILING);
	g_assert_nonnull(strstr(client->state_message, "host_whitelist"));
	/* The first request is the only one: nothing is asked after it. */
	g_assert_cmpuint(sab_count(fx->sab, "hostname-denied", FALSE), ==, 1);
}

/* Only the words in the first bytes of a 403 count, and any other 403 is
 * still a refused key. */
static void
test_other_403s_stay_a_refused_key(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisDownloadClientInfo) client = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *padding = g_strnfill(AXIS_HTTP_REFUSAL_BYTES + 100, 'x');
	g_autofree gchar *late = g_strconcat(padding, "Hostname verification failed", NULL);
	AxisDownloadTestResult result;

	sab_raw(fx->sab, "get_cats", 403, "text/plain", "Forbidden");
	g_assert_true(axis_downloads_test_client(fx->downloads, fx->client_id, &result, &error));
	g_assert_nonnull(strstr(result.note, "refused"));
	g_assert_null(strstr(result.note, "host name"));
	axis_download_test_result_clear(&result);
	g_assert_true(axis_downloads_get_client(fx->downloads, fx->client_id, &client, &error));
	g_assert_cmpint(client->state, ==, AXIS_DOWNLOAD_CLIENT_REFUSED);
	g_clear_pointer(&client, axis_download_client_info_free);

	sab_raw(fx->sab, "get_cats", 403, "text/plain", late);
	g_assert_true(axis_downloads_test_client(fx->downloads, fx->client_id, &result, &error));
	g_assert_null(strstr(result.note, "host name"));
	axis_download_test_result_clear(&result);
	g_assert_true(axis_downloads_get_client(fx->downloads, fx->client_id, &client, &error));
	g_assert_cmpint(client->state, ==, AXIS_DOWNLOAD_CLIENT_REFUSED);
}

/* Add: the host-name refusal fails the send, as a failure that is not a key. */
static void
test_add_hostname_check_is_not_a_refused_key(Fixture *fx, gconstpointer data)
{
	g_autoptr(GBytes) nzb = g_bytes_new_static(NZB_TEXT, strlen(NZB_TEXT));
	g_autoptr(GError) error = NULL;
	AxisDownloadRequest request;
	gint64 id = 0;

	sab_set_mode(fx->sab, SAB_HOSTNAME);
	memset(&request, 0, sizeof(request));
	request.client_id = fx->client_id;
	request.protocol = AXIS_DOWNLOAD_USENET;
	request.file = nzb;
	request.release_name = "Some.Film.2020.1080p";
	g_assert_false(axis_downloads_add(fx->downloads, &request, &id, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_FAILED);
	g_assert_nonnull(strstr(error->message, "SABnzbd refused the host name Axis used for it (127.0.0.1)"));
	g_assert_nonnull(strstr(error->message, "host_whitelist"));
	assert_state(fx, id, "failed");
	g_assert_cmpuint(fx->sab->jobs->len, ==, 0);
}

/* ── Add (2.2) ──────────────────────────────────────────────────── */

/* Spec: Add. The upload, field by field. */
static void
test_add_uploads_the_release(Fixture *fx, gconstpointer data)
{
	gint64 id = add_release(fx, "Some.Film.2020.1080p.WEB-DL");
	g_autofree gchar *ref = ref_of(fx, id);
	g_autofree gchar *cat = sab_last_add(fx->sab, "cat");
	g_autofree gchar *priority = sab_last_add(fx->sab, "priority");
	g_autofree gchar *nzbname = sab_last_add(fx->sab, "nzbname");
	g_autofree gchar *mode = sab_last_add(fx->sab, "mode");
	g_autofree gchar *output = sab_last_add(fx->sab, "output");
	g_autofree gchar *apikey = sab_last_add(fx->sab, "apikey");
	g_autofree gchar *file_name = sab_last_add(fx->sab, "@file_name");
	g_autofree gchar *file_type = sab_last_add(fx->sab, "@file_type");
	g_autofree gchar *well_formed = sab_last_add(fx->sab, "@multipart");
	g_autoptr(GBytes) file = sab_last_file(fx->sab);
	g_autoptr(AxisDownloadInfo) info = NULL;
	g_autoptr(GError) error = NULL;

	assert_state(fx, id, "queued");
	g_assert_cmpstr(ref, ==, "SABnzbd_nzo_t0001");
	g_assert_cmpstr(mode, ==, "addfile");
	g_assert_cmpstr(output, ==, "json");
	g_assert_cmpstr(apikey, ==, KEY);
	g_assert_cmpstr(cat, ==, "axis");
	g_assert_cmpstr(priority, ==, "1");	/* high */
	g_assert_cmpstr(nzbname, ==, "Some.Film.2020.1080p.WEB-DL");
	g_assert_cmpstr(file_name, ==, "Some.Film.2020.1080p.WEB-DL.nzb");
	g_assert_cmpstr(file_type, ==, "application/x-nzb");
	g_assert_cmpstr(well_formed, ==, "yes");
	/* The release as it was fetched, byte for byte. */
	g_assert_nonnull(file);
	g_assert_cmpuint(g_bytes_get_size(file), ==, strlen(NZB_TEXT));
	g_assert_cmpint(memcmp(g_bytes_get_data(file, NULL), NZB_TEXT, strlen(NZB_TEXT)), ==, 0);
	g_assert_cmpuint(sab_count(fx->sab, "addfile", FALSE), ==, 1);
	g_assert_true(axis_downloads_get(fx->downloads, id, &info, &error));
	g_assert_cmpstr(info->client_ref, ==, "SABnzbd_nzo_t0001");
	g_assert_cmpstr(info->title_label, ==, "Some Film (2020)");
	g_assert_null(info->info_hash);
	g_assert_cmpstr(info->client_name, ==, "Usenet");
}

static void
test_add_priorities(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *base = axis_test_plex_base(fx->sab->server);
	static const AxisDownloadPriority priorities[] = {
		AXIS_DOWNLOAD_PRIORITY_LOW, AXIS_DOWNLOAD_PRIORITY_NORMAL, AXIS_DOWNLOAD_PRIORITY_HIGH
	};
	static const gchar *const wanted[] = { "-1", "0", "1" };
	guint i;

	for (i = 0; i < G_N_ELEMENTS(priorities); i++) {
		g_autofree gchar *name = g_strdup_printf("Client %u", i);
		g_autofree gchar *release = g_strdup_printf("Release.%u", i);
		g_autofree gchar *priority = NULL;
		g_autoptr(GError) error = NULL;
		g_autoptr(GBytes) nzb = g_bytes_new_static(NZB_TEXT, strlen(NZB_TEXT));
		AxisDownloadRequest request;
		gint64 client = add_client_at(fx, name, base, NULL, KEY, priorities[i]);

		memset(&request, 0, sizeof(request));
		request.client_id = client;
		request.protocol = AXIS_DOWNLOAD_USENET;
		request.file = nzb;
		request.release_name = release;
		g_assert_true(axis_downloads_add(fx->downloads, &request, NULL, &error));
		g_assert_no_error(error);
		priority = sab_last_add(fx->sab, "priority");
		g_assert_cmpstr(priority, ==, wanted[i]);
	}
}

/* The upload's file name is made here, not copied from the release. */
static void
test_add_file_name_is_made_here(Fixture *fx, gconstpointer data)
{
	static const gchar *const names[] = {
		"Odd \"name\" / with ..\\ stuff; and \xc3\xa9",
		"..hidden",
		"%s%n",
		NULL
	};
	guint i;

	for (i = 0; names[i] != NULL; i++) {
		g_autofree gchar *file_name = NULL;
		g_autofree gchar *nzbname = NULL;
		g_autofree gchar *well_formed = NULL;
		const gchar *p;
		gint64 id = add_release(fx, names[i]);

		file_name = sab_last_add(fx->sab, "@file_name");
		nzbname = sab_last_add(fx->sab, "nzbname");
		well_formed = sab_last_add(fx->sab, "@multipart");
		g_assert_cmpstr(well_formed, ==, "yes");
		g_assert_true(g_str_has_suffix(file_name, ".nzb"));
		g_assert_cmpint(file_name[0], !=, '.');
		g_assert_cmpuint(strlen(file_name), <=, 104);
		for (p = file_name; *p != '\0'; p++)
			g_assert_true(g_ascii_isalnum(*p) || *p == '.' || *p == '_' || *p == '-');
		/* The job's name is the release's, whole: how a lost answer is matched. */
		g_assert_nonnull(nzbname);
		assert_state(fx, id, "queued");
	}
	/* A long name is cut. */
	{
		g_autofree gchar *long_name = g_strnfill(250, 'a');
		g_autofree gchar *file_name = NULL;

		add_release(fx, long_name);
		file_name = sab_last_add(fx->sab, "@file_name");
		g_assert_cmpuint(strlen(file_name), ==, 104);
	}
}

/* The boundary is random and is never one that occurs in the file. */
static void
test_add_boundary_is_not_in_the_release(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *first = NULL;
	g_autofree gchar *second = NULL;
	g_autofree gchar *boundary = NULL;
	g_autofree gchar *retried = NULL;
	g_autofree gchar *payload = NULL;
	g_autoptr(GBytes) file = NULL;
	g_autoptr(GBytes) nzb = NULL;
	g_autoptr(GError) error = NULL;
	AxisDownloadRequest request;
	guint i;

	/* Two uploads, two boundaries. */
	add_release(fx, "Boundary.One");
	first = sab_last_add(fx->sab, "@boundary");
	add_release(fx, "Boundary.Two");
	second = sab_last_add(fx->sab, "@boundary");
	g_assert_nonnull(first);
	g_assert_cmpstr(first, !=, second);
	g_assert_cmpuint(strlen(first), >=, 32);

	/* A release that holds the boundary tried first (forced through the
	 * kind's test hook, since no random draw can be aimed): the kind must
	 * try another, and the release must arrive whole, parsed as the one
	 * part it is, with the framing intact. */
	boundary = g_strdup("axis-clashing-boundary-0123456789abcdef");
	retried = g_strdup("axis-the-second-boundary-fedcba9876543210");
	{
		const gchar *const forced[] = { boundary, retried, NULL };

		payload = g_strdup_printf("<nzb><!-- --%s --></nzb>\r\n--%s--\r\n--%s\r\n", boundary, boundary, boundary);
		axis_download_sabnzbd_set_boundaries(forced);
	}
	nzb = g_bytes_new(payload, strlen(payload));
	memset(&request, 0, sizeof(request));
	request.protocol = AXIS_DOWNLOAD_USENET;
	request.file = nzb;
	request.release_name = "Boundary.Clash";
	g_assert_true(axis_downloads_add(fx->downloads, &request, NULL, &error));
	g_assert_no_error(error);
	g_free(second);
	second = sab_last_add(fx->sab, "@boundary");
	g_assert_cmpstr(second, ==, retried);
	g_assert_null(strstr(payload, second));
	file = sab_last_file(fx->sab);
	g_assert_nonnull(file);
	g_assert_cmpuint(g_bytes_get_size(file), ==, strlen(payload));
	g_assert_cmpint(memcmp(g_bytes_get_data(file, NULL), payload, strlen(payload)), ==, 0);
	g_free(second);
	second = sab_last_add(fx->sab, "@multipart");
	g_assert_cmpstr(second, ==, "yes");

	/* When every try clashes (the kind tries sixteen), nothing is sent at all
	 * and the record is failed. */
	{
		GString *all = g_string_new("<nzb>");
		g_autofree gchar *many = NULL;
		const gchar *forced[18];
		gchar *names[17];
		guint sent_before = sab_count(fx->sab, "addfile", FALSE);
		gint64 id;

		for (i = 0; i < 17; i++) {
			names[i] = g_strdup_printf("axis-clash-%02u-xxxxxxxxxxxxxxxx", i);
			forced[i] = names[i];
			g_string_append(all, names[i]);
		}
		forced[17] = NULL;
		g_string_append(all, "</nzb>");
		many = g_string_free(all, FALSE);
		g_bytes_unref(g_steal_pointer(&nzb));
		nzb = g_bytes_new(many, strlen(many));
		axis_download_sabnzbd_set_boundaries(forced);
		request.file = nzb;
		request.release_name = "Boundary.Never";
		g_clear_error(&error);
		id = 0;
		g_assert_false(axis_downloads_add(fx->downloads, &request, &id, &error));
		g_assert_nonnull(error);
		g_assert_cmpuint(sab_count(fx->sab, "addfile", FALSE), ==, sent_before);
		assert_state(fx, id, "failed");
		for (i = 0; i < 17; i++)
			g_free(names[i]);
	}
}

/* Spec: Failure inside a successful answer */
static void
test_add_refused_inside_a_200(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *failed = fixture_text("addfile-failed.json");
	g_autofree gchar *problem = NULL;
	g_autoptr(GError) error = NULL;
	gint64 id;

	sab_raw(fx->sab, "addfile", 200, "application/json", failed);
	id = add_release_at(fx, "Some.Film.2020.1080p", &error);
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_REFUSED);
	g_assert_nonnull(strstr(error->message, "Cannot read the file: it is not an NZB (<html>)"));
	g_clear_error(&error);
	g_assert_cmpint(id, >, 0);
	/* Certainly not sent: failed with SABnzbd's own words, and not kept to be matched. */
	assert_state(fx, id, "failed");
	problem = q(fx, "SELECT problem FROM download WHERE id = %" G_GINT64_FORMAT, id);
	g_assert_true(g_str_has_prefix(problem, "Not sent: SABnzbd refused it: Cannot read the file"));
	g_assert_cmpuint(sab_count(fx->sab, "addfile", FALSE), ==, 1);
	/* A poll does not send it again. */
	poll(fx);
	g_assert_cmpuint(sab_count(fx->sab, "addfile", FALSE), ==, 1);
}

/* Not exactly one identifier is a refusal too. */
static void
test_add_needs_exactly_one_identifier(Fixture *fx, gconstpointer data)
{
	static const gchar *const fixtures[] = { "addfile-no-ids.json", "addfile-two-ids.json", NULL };
	guint i;

	for (i = 0; fixtures[i] != NULL; i++) {
		g_autoptr(GError) error = NULL;
		gint64 id;

		sab_raw_fixture(fx->sab, "addfile", fixtures[i]);
		id = add_release_at(fx, "Some.Film.2020.1080p", &error);
		g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_REFUSED);
		assert_state(fx, id, "failed");
		assert_q(fx, NULL, "SELECT client_ref FROM download WHERE state = 'failed' LIMIT 1");
		axis_test_db_exec(fx->t->db, "DELETE FROM download");
	}
}

static void
test_add_with_a_wrong_key(Fixture *fx, gconstpointer data)
{
	gint64 wrong = add_client(fx, "Wrong", WRONG_KEY);
	g_autoptr(GBytes) nzb = g_bytes_new_static(NZB_TEXT, strlen(NZB_TEXT));
	g_autoptr(GError) error = NULL;
	AxisDownloadRequest request;
	gint64 id = 0;

	memset(&request, 0, sizeof(request));
	request.client_id = wrong;
	request.protocol = AXIS_DOWNLOAD_USENET;
	request.file = nzb;
	request.release_name = "Some.Film.2020.1080p";
	g_assert_false(axis_downloads_add(fx->downloads, &request, &id, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_AUTH);
	g_assert_null(strstr(error->message, WRONG_KEY));
	assert_state(fx, id, "failed");
	g_assert_cmpuint(sab_count(fx->sab, "badkey addfile", FALSE), ==, 1);
	g_assert_cmpuint(sab_count(fx->sab, "addfile", FALSE), ==, 0);
	g_assert_cmpuint(fx->sab->jobs->len, ==, 0);
}

/* Magnets and torrents are refused before any request. */
static void
test_add_refuses_other_protocols(Fixture *fx, gconstpointer data)
{
	const AxisDownloadKindInfo *kind = axis_download_sabnzbd_kind();
	g_autoptr(GBytes) nzb = g_bytes_new_static(NZB_TEXT, strlen(NZB_TEXT));
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
	release.protocol = AXIS_DOWNLOAD_TORRENT;
	release.magnet = "magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567";
	release.name = "Some.Film.2020";
	g_assert_false(kind->add(kind, &conn, &release, "axis", AXIS_DOWNLOAD_PRIORITY_NORMAL, &ref, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID);
	g_clear_error(&error);
	release.protocol = AXIS_DOWNLOAD_TORRENT;
	release.magnet = NULL;
	release.file = nzb;
	g_assert_false(kind->add(kind, &conn, &release, "axis", AXIS_DOWNLOAD_PRIORITY_NORMAL, &ref, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID);
	g_clear_error(&error);
	release.protocol = AXIS_DOWNLOAD_USENET;
	release.file = empty;
	g_assert_false(kind->add(kind, &conn, &release, "axis", AXIS_DOWNLOAD_PRIORITY_NORMAL, &ref, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID);
	g_clear_error(&error);
	release.file = NULL;
	release.magnet = "magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567";
	g_assert_false(kind->add(kind, &conn, &release, "axis", AXIS_DOWNLOAD_PRIORITY_NORMAL, &ref, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID);
	g_clear_error(&error);

	/* Through the layer: a magnet link to this client is refused too. */
	memset(&request, 0, sizeof(request));
	request.client_id = fx->client_id;
	request.protocol = AXIS_DOWNLOAD_TORRENT;
	request.magnet = "magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567";
	request.release_name = "Some.Film.2020";
	g_assert_false(axis_downloads_add(fx->downloads, &request, NULL, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID);
	g_clear_error(&error);
	request.client_id = 0;
	g_assert_false(axis_downloads_add(fx->downloads, &request, NULL, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_NO_CLIENT);
	assert_q(fx, "0", "SELECT count(*) FROM download");
	g_assert_cmpuint(axis_test_plex_n_requests(fx->sab->server), ==, 0);
}

/* ── List (2.3) and the other categories (3.3) ──────────────────── */

/* Task 3.3: only Axis's category is looked at, only what Axis added acted on */
static void
test_other_categories_are_never_touched(Fixture *fx, gconstpointer data)
{
	AxisDownloadPollReport report;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *want_cat = NULL;
	g_autofree gchar *want_category = NULL;
	g_autofree gchar *limit = NULL;
	g_autofree gchar *start = NULL;
	gint64 id = add_release(fx, "Some.Film.2020.1080p");
	g_autofree gchar *ref = ref_of(fx, id);

	/* Another category (a person's), the same category by another program
	 * (Radarr's own, which Axis did not add), in the queue and the history. */
	sab_put(fx->sab, "SABnzbd_nzo_man001", "Holiday.Videos", "manual", "Downloading", FALSE, 100, 50, NULL, NULL);
	sab_put(fx->sab, "SABnzbd_nzo_man002", "Holiday.Videos.Two", "manual", "Completed", TRUE, 100, 0,
		"/downloads/complete/manual/Holiday.Videos.Two", NULL);
	sab_put(fx->sab, "SABnzbd_nzo_oth001", "Other.Film.2018", "axis", "Downloading", FALSE, 100, 50, NULL, NULL);
	sab_put(fx->sab, "SABnzbd_nzo_oth002", "Other.Film.2017", "axis", "Completed", TRUE, 100, 0,
		"/downloads/complete/axis/Other.Film.2017", NULL);

	report = poll(fx);
	g_assert_true(report.reached);
	g_assert_cmpuint(report.listed, ==, 3);		/* Axis's category only: its own and two of another program's */
	g_assert_cmpuint(report.ignored, ==, 2);	/* the other program's, never touched */
	assert_q(fx, "1", "SELECT count(*) FROM download");
	assert_q(fx, "0", "SELECT count(*) FROM download WHERE client_ref LIKE 'SABnzbd_nzo_man%' OR client_ref LIKE 'SABnzbd_nzo_oth%'");

	/* What the kind asked for: the category, a start and a limit. */
	want_cat = sab_last_query(fx->sab, "cat");
	want_category = sab_last_query(fx->sab, "category");
	limit = sab_last_query(fx->sab, "limit");
	start = sab_last_query(fx->sab, "start");
	g_assert_cmpstr(want_cat, ==, "axis");
	g_assert_cmpstr(want_category, ==, "axis");
	g_assert_cmpstr(limit, ==, "500");
	g_assert_cmpstr(start, ==, "0");

	/* Removal of what Axis did not add is refused, and nothing is sent. */
	g_assert_false(axis_downloads_remove_ref(fx->downloads, fx->client_id, "SABnzbd_nzo_man001", TRUE, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_NOT_FOUND);
	g_clear_error(&error);
	g_assert_false(axis_downloads_remove_ref(fx->downloads, fx->client_id, "SABnzbd_nzo_oth001", TRUE, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_NOT_FOUND);
	g_clear_error(&error);
	g_assert_false(axis_downloads_remove_ref(fx->downloads, fx->client_id, "SABnzbd_nzo_man002", TRUE, &error));
	g_clear_error(&error);
	g_assert_cmpuint(sab_count(fx->sab, "queue:delete", TRUE), ==, 0);
	g_assert_cmpuint(sab_count(fx->sab, "history:delete", TRUE), ==, 0);
	g_assert_true(sab_has(fx->sab, "SABnzbd_nzo_man001"));
	g_assert_true(sab_has(fx->sab, "SABnzbd_nzo_oth001"));

	/* Its own: removed, and only it. */
	g_assert_true(axis_downloads_remove_ref(fx->downloads, fx->client_id, ref, TRUE, &error));
	g_assert_no_error(error);
	g_assert_false(sab_has(fx->sab, ref));
	g_assert_true(sab_has(fx->sab, "SABnzbd_nzo_man001"));
	g_assert_true(sab_has(fx->sab, "SABnzbd_nzo_oth001"));
	g_assert_true(sab_has(fx->sab, "SABnzbd_nzo_man002"));
	g_assert_true(sab_has(fx->sab, "SABnzbd_nzo_oth002"));
	poll(fx);
	poll(fx);
	poll(fx);
	poll(fx);
	g_assert_cmpuint(sab_count(fx->sab, "queue:delete", TRUE) + sab_count(fx->sab, "history:delete", TRUE), ==, 1);
	assert_q(fx, "1", "SELECT count(*) FROM download");
}

/* Task 2.3, 3.1, 3.4: the fixtures through the whole layer */
static void
test_fixture_answers_through_the_layer(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *place = g_build_filename(fx->disk, "downloads", "complete", "axis", "Some.Film.2020.1080p.WEB-DL", NULL);
	g_autofree gchar *film = g_build_filename(place, "film.mkv", NULL);
	g_autoptr(AxisDownloadInfo) info = NULL;
	g_autoptr(GError) error = NULL;
	AxisDownloadPollReport report;
	gint64 downloading;
	gint64 queued;
	gint64 paused;
	gint64 fetching;
	gint64 propagating;
	gint64 grabbing;
	gint64 completed;
	gint64 failed;
	gint64 verifying;
	gint64 moving;
	gint64 folderless;

	downloading = insert_record(fx, "SABnzbd_nzo_dl0001", "Some.Film.2020.1080p.WEB-DL", "queued");
	queued = insert_record(fx, "SABnzbd_nzo_q00002", "Another.Film.2019.720p", "queued");
	paused = insert_record(fx, "SABnzbd_nzo_pa0003", "Some.Series.S01E01.720p", "queued");
	fetching = insert_record(fx, "SABnzbd_nzo_fe0004", "Fetching.Extra.Blocks.2018", "queued");
	propagating = insert_record(fx, "SABnzbd_nzo_pr0005", "Too.New.To.Fetch.2024", "downloading");
	grabbing = insert_record(fx, "SABnzbd_nzo_gr0006", "Grabbing.NZB.From.Indexer", "queued");
	sab_raw_fixture(fx->sab, "queue", "queue.json");
	sab_raw_fixture(fx->sab, "history", "history-empty.json");
	report = poll(fx);
	g_assert_true(report.reached);
	g_assert_cmpuint(report.listed, ==, 6);		/* the manual job is not Axis's category */
	assert_state(fx, downloading, "downloading");
	assert_state(fx, queued, "queued");
	assert_state(fx, paused, "paused");
	assert_state(fx, fetching, "downloading");
	assert_state(fx, propagating, "queued");
	assert_state(fx, grabbing, "downloading");
	g_assert_true(axis_downloads_get(fx->downloads, downloading, &info, &error));
	g_assert_cmpint(info->size_bytes, ==, megabytes(4120.55));
	g_assert_cmpint(info->done_bytes, ==, megabytes(4120.55) - megabytes(1581.05));
	g_assert_cmpint(info->eta_s, ==, 316);
	g_assert_cmpfloat(axis_download_info_progress(info), >, 0.6);
	g_assert_cmpfloat(axis_download_info_progress(info), <, 0.65);
	g_clear_pointer(&info, axis_download_info_free);

	/* The history: the post-processing statuses are downloading at 100 %,
	 * a Completed one is completed only with its folder, which is mapped. */
	completed = insert_record(fx, "SABnzbd_nzo_cp0001", "Some.Film.2020.1080p.WEB-DL", "downloading");
	failed = insert_record(fx, "SABnzbd_nzo_fl0001", "Bad.Release.2021.720p", "downloading");
	verifying = insert_record(fx, "SABnzbd_nzo_vf0001", "Post.Verifying.2020", "downloading");
	moving = insert_record(fx, "SABnzbd_nzo_mv0001", "Post.Moving.2020", "queued");
	folderless = insert_record(fx, "SABnzbd_nzo_cp0003", "No.Folder.Release", "downloading");
	axis_test_db_exec(fx->t->db, "DELETE FROM download WHERE client_ref IN ('SABnzbd_nzo_dl0001', 'SABnzbd_nzo_q00002',"
		" 'SABnzbd_nzo_pa0003', 'SABnzbd_nzo_fe0004', 'SABnzbd_nzo_pr0005', 'SABnzbd_nzo_gr0006')");
	sab_raw_fixture(fx->sab, "queue", "queue-empty.json");
	sab_raw_fixture(fx->sab, "history", "history.json");
	axis_test_storage_mkdir(place);
	axis_test_storage_write(film, "film");
	add_map(fx);
	report = poll(fx);
	g_assert_cmpuint(report.completed, ==, 1);
	g_assert_cmpuint(report.failed, ==, 1);
	g_assert_cmpuint(report.waiting, ==, 1);	/* Completed, with no folder to say where */
	assert_state(fx, completed, "completed");
	assert_state(fx, failed, "failed");
	assert_state(fx, verifying, "downloading");
	assert_state(fx, moving, "downloading");
	assert_state(fx, folderless, "downloading");
	g_assert_true(axis_downloads_get(fx->downloads, completed, &info, &error));
	g_assert_cmpint(info->volume_id, ==, fx->volume_id);
	g_assert_cmpstr(info->rel_path, ==, "downloads/complete/axis/Some.Film.2020.1080p.WEB-DL");
	g_assert_cmpstr(info->client_path, ==, "/downloads/complete/axis/Some.Film.2020.1080p.WEB-DL");
	g_clear_pointer(&info, axis_download_info_free);
	g_assert_true(axis_downloads_get(fx->downloads, failed, &info, &error));
	g_assert_cmpstr(info->message, ==, "Aborted, cannot be completed - https://sabnzbd.org/not-complete");
	g_clear_pointer(&info, axis_download_info_free);
	g_assert_true(axis_downloads_get(fx->downloads, verifying, &info, &error));
	g_assert_cmpint(info->size_bytes, ==, 1073741824);
	g_assert_cmpint(info->done_bytes, ==, 1073741824);
	g_assert_cmpfloat(axis_download_info_progress(info), ==, 1.0);
	g_assert_null(info->rel_path);
	g_clear_pointer(&info, axis_download_info_free);
	g_assert_true(axis_downloads_get(fx->downloads, folderless, &info, &error));
	g_assert_nonnull(strstr(info->problem, "no path"));
	g_clear_pointer(&info, axis_download_info_free);
}

/* A list that cannot be read changes nothing (the half missing would look gone). */
static void
test_unreadable_lists_change_nothing(Fixture *fx, gconstpointer data)
{
	AxisDownloadPollReport report;
	g_autoptr(AxisDownloadClientInfo) client = NULL;
	g_autoptr(GError) error = NULL;
	gint64 id = add_release(fx, "Some.Film.2020.1080p");
	g_autofree gchar *ref = ref_of(fx, id);
	static const gchar *const fixtures[] = {
		"queue-no-slots.json", "not-an-object.json", "truncated.json", "wrong-key.json", NULL
	};
	guint i;

	sab_status(fx->sab, ref, "Downloading", 500);
	poll(fx);
	assert_state(fx, id, "downloading");
	for (i = 0; fixtures[i] != NULL; i++) {
		sab_raw_fixture(fx->sab, "queue", fixtures[i]);
		report = poll(fx);
		g_assert_false(report.reached);
	}
	sab_clear_raw(fx->sab, "queue");
	/* The history half. */
	for (i = 0; i < 4; i++) {
		static const gchar *const bad[] = { "history-no-slots.json", "not-an-object.json", "truncated.json", "wrong-key.json" };

		sab_raw_fixture(fx->sab, "history", bad[i]);
		report = poll(fx);
		g_assert_false(report.reached);
	}
	assert_state(fx, id, "downloading");
	assert_q(fx, "0", "SELECT missing_polls FROM download");
	/* A key that stopped working is shown as refused. */
	sab_raw_fixture(fx->sab, "queue", "wrong-key.json");
	poll(fx);
	g_assert_true(axis_downloads_get_client(fx->downloads, fx->client_id, &client, &error));
	g_assert_cmpint(client->state, ==, AXIS_DOWNLOAD_CLIENT_REFUSED);
	g_clear_pointer(&client, axis_download_client_info_free);
	/* And everything is as before once it answers. */
	sab_clear_raw(fx->sab, "queue");
	sab_clear_raw(fx->sab, "history");
	report = poll(fx);
	g_assert_true(report.reached);
	assert_state(fx, id, "downloading");
}

/* ── Post-processing, completion, failure (3.4) ─────────────────── */

/* Spec: Post-processing; Completed */
static void
test_post_processing_then_completed(Fixture *fx, gconstpointer data)
{
	static const gchar *const steps[] = { "Queued", "Verifying", "Repairing", "Extracting", "Moving", "Running", "QuickCheck", NULL };
	g_autofree gchar *place = g_build_filename(fx->disk, "downloads", "complete", "axis", "Some.Film.2020.1080p", NULL);
	g_autoptr(AxisDownloadInfo) info = NULL;
	g_autoptr(AxisDownloadClientInfo) client = NULL;
	g_autoptr(GError) error = NULL;
	AxisDownloadPollReport report;
	gint64 id = add_release(fx, "Some.Film.2020.1080p");
	g_autofree gchar *ref = ref_of(fx, id);
	guint i;

	/* Downloading. */
	sab_status(fx->sab, ref, "Downloading", 400);
	poll(fx);
	assert_state(fx, id, "downloading");
	g_assert_true(axis_downloads_get(fx->downloads, id, &info, &error));
	g_assert_cmpfloat(axis_download_info_progress(info), ==, 0.6);
	g_clear_pointer(&info, axis_download_info_free);

	/* Every status of the work after the download: still downloading, all
	 * of it, nothing located; even when SABnzbd already names a folder. */
	sab_finish(fx->sab, ref, "Verifying", NULL, NULL);
	for (i = 0; steps[i] != NULL; i++) {
		sab_finish(fx->sab, ref, steps[i], i % 2 == 0 ? "/downloads/complete/axis/Some.Film.2020.1080p" : NULL, NULL);
		report = poll(fx);
		g_assert_cmpuint(report.completed, ==, 0);
		assert_state(fx, id, "downloading");
		g_assert_true(axis_downloads_get(fx->downloads, id, &info, &error));
		g_assert_cmpfloat(axis_download_info_progress(info), ==, 1.0);
		g_assert_null(info->rel_path);
		g_assert_null(info->problem);
		g_clear_pointer(&info, axis_download_info_free);
	}
	assert_q(fx, NULL, "SELECT rel_path FROM download");

	/* Completed, with its folder but no mapping: it waits, and says why. */
	sab_finish(fx->sab, ref, "Completed", "/downloads/complete/axis/Some.Film.2020.1080p", NULL);
	report = poll(fx);
	g_assert_cmpuint(report.waiting, ==, 1);
	assert_state(fx, id, "downloading");
	g_assert_true(axis_downloads_get(fx->downloads, id, &info, &error));
	g_assert_nonnull(strstr(info->problem, "no path mapping"));
	g_clear_pointer(&info, axis_download_info_free);

	/* Mapped, but not there yet: waits. */
	add_map(fx);
	poll(fx);
	assert_state(fx, id, "downloading");
	g_assert_true(axis_downloads_get(fx->downloads, id, &info, &error));
	g_assert_nonnull(strstr(info->problem, "nothing is at the mapped place"));
	g_clear_pointer(&info, axis_download_info_free);

	/* There: completed, at the location the mapping gives. */
	axis_test_storage_mkdir(place);
	report = poll(fx);
	g_assert_cmpuint(report.completed, ==, 1);
	assert_state(fx, id, "completed");
	g_assert_true(axis_downloads_get(fx->downloads, id, &info, &error));
	g_assert_cmpint(info->volume_id, ==, fx->volume_id);
	g_assert_cmpstr(info->rel_path, ==, "downloads/complete/axis/Some.Film.2020.1080p");
	g_assert_null(info->problem);
	g_clear_pointer(&info, axis_download_info_free);
	g_assert_true(axis_downloads_get_client(fx->downloads, fx->client_id, &client, &error));
	g_assert_null(client->problem);
}

static void
test_failed_with_the_clients_reason(Fixture *fx, gconstpointer data)
{
	AxisDownloadPollReport report;
	g_autofree gchar *message = NULL;
	gint64 id = add_release(fx, "Some.Film.2020.1080p");
	g_autofree gchar *ref = ref_of(fx, id);

	sab_status(fx->sab, ref, "Downloading", 400);
	poll(fx);
	sab_finish(fx->sab, ref, "Failed", "/downloads/incomplete/x", "Aborted, cannot be completed - see\\n the log");
	report = poll(fx);
	g_assert_cmpuint(report.failed, ==, 1);
	assert_state(fx, id, "failed");
	message = q(fx, "SELECT message FROM download WHERE id = %" G_GINT64_FORMAT, id);
	g_assert_cmpstr(message, ==, "Aborted, cannot be completed - see the log");
}

/* ── Unknown status words ───────────────────────────────────────── */

static void
test_unknown_status_keeps_the_last_state(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisDownloadInfo) info = NULL;
	g_autoptr(GError) error = NULL;
	gint64 one = add_release(fx, "Some.Film.2020.1080p");
	gint64 two = add_release(fx, "Another.Film.2019.720p");
	g_autofree gchar *one_ref = ref_of(fx, one);
	g_autofree gchar *two_ref = ref_of(fx, two);

	sab_status(fx->sab, one_ref, "Downloading", 400);
	sab_status(fx->sab, two_ref, "Paused", 900);
	poll(fx);
	assert_state(fx, one, "downloading");
	assert_state(fx, two, "paused");

	/* A word nobody knows: each keeps the state it had, and the word is
	 * logged once however often it is seen, and by however many jobs. */
	sab_status(fx->sab, one_ref, "Wibbling", 300);
	sab_status(fx->sab, two_ref, "Wibbling", 800);
	poll(fx);
	poll(fx);
	poll(fx);
	assert_state(fx, one, "downloading");
	assert_state(fx, two, "paused");
	g_assert_cmpuint(logged("*Wibbling*"), ==, 1);
	/* Its figures still move. */
	g_assert_true(axis_downloads_get(fx->downloads, one, &info, &error));
	g_assert_cmpfloat(axis_download_info_progress(info), ==, 0.7);
	g_clear_pointer(&info, axis_download_info_free);

	/* Another word is logged once too. */
	sab_status(fx->sab, one_ref, "Wobbling", 300);
	poll(fx);
	poll(fx);
	assert_state(fx, one, "downloading");
	g_assert_cmpuint(logged("*Wobbling*"), ==, 1);
	g_assert_cmpuint(logged("*Wibbling*"), ==, 1);

	/* A known word again, and the state follows it. */
	sab_status(fx->sab, one_ref, "Paused", 300);
	poll(fx);
	assert_state(fx, one, "paused");

	/* In the history: a job in post-processing stays downloading at 100 %,
	 * never completed, whatever word follows. */
	sab_status(fx->sab, two_ref, "Downloading", 100);
	poll(fx);
	assert_state(fx, two, "downloading");
	sab_finish(fx->sab, two_ref, "Extracting", NULL, NULL);
	poll(fx);
	sab_finish(fx->sab, two_ref, "Wibbling", "/downloads/complete/axis/Another.Film.2019.720p", NULL);
	poll(fx);
	assert_state(fx, two, "downloading");
	g_assert_true(axis_downloads_get(fx->downloads, two, &info, &error));
	g_assert_cmpfloat(axis_download_info_progress(info), ==, 1.0);
	g_clear_pointer(&info, axis_download_info_free);
	g_assert_cmpuint(logged("*Wibbling*"), ==, 1);

	/* After a restart nothing is remembered: an unknown word in the queue is queued. */
	axis_download_sabnzbd_forget();
	sab_put(fx->sab, "SABnzbd_nzo_dl0001", "Fresh.Job", "axis", "Wibbling", FALSE, 100, 100, NULL, NULL);
	insert_record(fx, "SABnzbd_nzo_dl0001", "Fresh.Job", "downloading");
	poll(fx);
	assert_q(fx, "queued", "SELECT state FROM download WHERE client_ref = 'SABnzbd_nzo_dl0001'");
}

/* The fixtures' unknown words, through the layer. */
static void
test_unknown_status_fixtures(Fixture *fx, gconstpointer data)
{
	gint64 one = insert_record(fx, "SABnzbd_nzo_dl0001", "Some.Film.2020.1080p.WEB-DL", "downloading");
	gint64 two = insert_record(fx, "SABnzbd_nzo_nw0002", "Never.Seen.Before", "paused");

	sab_raw_fixture(fx->sab, "queue", "queue-unknown-status.json");
	sab_raw_fixture(fx->sab, "history", "history-empty.json");
	poll(fx);
	/* No memory of either: the queue's placeholder (queued) is the state. */
	assert_state(fx, one, "queued");
	assert_state(fx, two, "queued");
	g_assert_cmpuint(logged("*Wibbling*"), ==, 1);
	g_assert_cmpuint(logged("*Wobbling*"), ==, 1);
	sab_raw_fixture(fx->sab, "queue", "queue-empty.json");
	sab_raw_fixture(fx->sab, "history", "history-unknown-status.json");
	poll(fx);
	/* In the history, the same words mean downloading, all of it. */
	assert_state(fx, one, "downloading");
	assert_state(fx, two, "downloading");
	g_assert_cmpuint(logged("*Wibbling*"), ==, 1);
}

/* ── Remove (2.4) ───────────────────────────────────────────────── */

static void
test_remove_from_the_queue_or_the_history(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	gint64 queued = add_release(fx, "Queue.Job");
	gint64 later = add_release(fx, "History.Job");
	gint64 third = add_release(fx, "Unknown.Place.Job");
	g_autofree gchar *queued_ref = ref_of(fx, queued);
	g_autofree gchar *later_ref = ref_of(fx, later);
	g_autofree gchar *third_ref = ref_of(fx, third);
	g_autofree gchar *want = NULL;

	sab_status(fx->sab, queued_ref, "Downloading", 500);
	sab_status(fx->sab, later_ref, "Downloading", 500);
	sab_status(fx->sab, third_ref, "Downloading", 500);
	poll(fx);
	sab_finish(fx->sab, later_ref, "Extracting", NULL, NULL);
	poll(fx);

	/* Last seen in the queue: the queue's call, with its files. */
	g_assert_true(axis_downloads_remove(fx->downloads, queued, TRUE, &error));
	g_assert_no_error(error);
	assert_state(fx, queued, "removed");
	g_assert_false(sab_has(fx->sab, queued_ref));
	want = g_strdup_printf("queue:delete:%s:del=1", queued_ref);
	g_assert_cmpuint(sab_count(fx->sab, want, FALSE), ==, 1);
	g_assert_cmpuint(sab_count(fx->sab, "history:delete", TRUE), ==, 0);

	/* Last seen in the history: the history's call, without its files. */
	g_assert_true(axis_downloads_remove(fx->downloads, later, FALSE, &error));
	g_assert_no_error(error);
	assert_state(fx, later, "removed");
	g_assert_false(sab_has(fx->sab, later_ref));
	{
		g_autofree gchar *history = g_strdup_printf("history:delete:%s:del=0", later_ref);
		g_autofree gchar *not_queue = g_strdup_printf("queue:delete:%s", later_ref);

		g_assert_cmpuint(sab_count(fx->sab, history, FALSE), ==, 1);
		g_assert_cmpuint(sab_count(fx->sab, not_queue, TRUE), ==, 0);
	}

	/* Nothing known of where it is (a restart): the queue first, which has
	 * it, and then nothing more. */
	axis_download_sabnzbd_forget();
	g_assert_true(axis_downloads_remove(fx->downloads, third, TRUE, &error));
	g_assert_no_error(error);
	g_assert_false(sab_has(fx->sab, third_ref));
	{
		g_autofree gchar *in_queue = g_strdup_printf("queue:delete:%s:del=1", third_ref);
		g_autofree gchar *in_history = g_strdup_printf("history:delete:%s", third_ref);

		g_assert_cmpuint(sab_count(fx->sab, in_queue, FALSE), ==, 1);
		g_assert_cmpuint(sab_count(fx->sab, in_history, TRUE), ==, 0);
	}
	g_assert_cmpuint(sab_bad_requests(fx->sab), ==, 0);
}

static void
test_remove_with_no_memory_tries_both(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	gint64 id = add_release(fx, "History.Job");
	g_autofree gchar *ref = ref_of(fx, id);
	g_autofree gchar *calls = NULL;
	g_autofree gchar *queue_call = g_strdup_printf("queue:delete:%s:del=0", ref);
	g_autofree gchar *history_call = g_strdup_printf("history:delete:%s:del=0", ref);

	sab_status(fx->sab, ref, "Downloading", 500);
	poll(fx);
	sab_finish(fx->sab, ref, "Moving", "/downloads/complete/axis/History.Job", NULL);
	axis_download_sabnzbd_forget();
	axis_test_plex_reset_requests(fx->sab->server);
	g_mutex_lock(&fx->sab->lock);
	g_ptr_array_set_size(fx->sab->calls, 0);
	g_mutex_unlock(&fx->sab->lock);

	/* The queue does not have it (it names nothing removed): then the history. */
	g_assert_true(axis_downloads_remove(fx->downloads, id, FALSE, &error));
	g_assert_no_error(error);
	assert_state(fx, id, "removed");
	g_assert_false(sab_has(fx->sab, ref));
	calls = sab_calls(fx->sab);
	g_assert_cmpuint(sab_count(fx->sab, queue_call, FALSE), ==, 1);
	g_assert_cmpuint(sab_count(fx->sab, history_call, FALSE), ==, 1);
	g_assert_true(strstr(calls, queue_call) < strstr(calls, history_call));
}

/* A refusal in a 200 from the queue's delete does not stop the history's. */
static void
test_remove_after_a_refusal_and_with_a_wrong_key(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	gint64 id = add_release(fx, "History.Job");
	g_autofree gchar *ref = ref_of(fx, id);
	g_autofree gchar *history_call = g_strdup_printf("history:delete:%s:del=1", ref);

	sab_status(fx->sab, ref, "Downloading", 500);
	axis_download_sabnzbd_forget();
	sab_finish(fx->sab, ref, "Moving", "/downloads/complete/axis/History.Job", NULL);
	sab_raw(fx->sab, "queue", 200, "application/json", "{\"status\": false, \"error\": \"Cannot do that\"}");
	g_assert_true(axis_downloads_remove(fx->downloads, id, TRUE, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(sab_count(fx->sab, history_call, FALSE), ==, 1);
	assert_state(fx, id, "removed");

	/* A key that no longer works is an authentication failure, and the
	 * download is not marked removed. */
	{
		gint64 other = add_release(fx, "Other.Job");
		g_autofree gchar *other_ref = ref_of(fx, other);

		sab_clear_raw(fx->sab, "queue");
		sab_raw_fixture(fx->sab, "queue", "wrong-key.json");
		g_assert_false(axis_downloads_remove(fx->downloads, other, TRUE, &error));
		g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_AUTH);
		assert_state(fx, other, "queued");
		g_assert_true(sab_has(fx->sab, other_ref));
	}
}

/* ── What SABnzbd deletes (fix-first-import) ────────────────────── */

/* A finished job of the made-up @name whose folder is on the test's disk
 * at SABnzbd's @storage, with a file in it. */
static gchar *
finished_job_on_disk(
	Fixture     *fx,
	const gchar *name,
	const gchar *status,
	gint64      *id_out,
	gchar      **ref_out
){
	gint64 id = add_release(fx, name);
	g_autofree gchar *ref = ref_of(fx, id);
	g_autofree gchar *storage = g_strdup_printf("/downloads/complete/axis/%s", name);
	gchar *folder = g_strconcat(fx->disk, storage, NULL);
	g_autofree gchar *file = g_build_filename(folder, "film.mkv", NULL);

	g_assert_cmpint(g_mkdir_with_parents(folder, 0755), ==, 0);
	g_assert_true(g_file_set_contents(file, "a film", -1, NULL));
	sab_status(fx->sab, ref, "Downloading", 500);
	poll(fx);
	sab_finish(fx->sab, ref, status, storage, g_strcmp0(status, "Failed") == 0 ? "Repair failed" : NULL);
	poll(fx);
	*id_out = id;
	*ref_out = g_steal_pointer(&ref);
	return folder;
}

/* Spec (downloaders): SABnzbd removes a job with its files: del_files=1 on
 * its history deletes the files of a job that failed, and of a job that
 * completed only the entry (seen from SABnzbd 5.1.3), so a completed job's
 * folder stays where it finished. */
static void
test_remove_keeps_a_completed_jobs_folder(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *done_ref = NULL;
	g_autofree gchar *failed_ref = NULL;
	g_autofree gchar *done_folder = NULL;
	g_autofree gchar *failed_folder = NULL;
	g_autofree gchar *call = NULL;
	gint64 done = 0;
	gint64 failed = 0;

	add_map(fx);
	sab_set_disk(fx->sab, fx->disk);
	done_folder = finished_job_on_disk(fx, "Marrowfield.Lanterns.2011-MADEUP", "Completed", &done, &done_ref);
	failed_folder = finished_job_on_disk(fx, "Bramblewick.Tides.2010-MADEUP", "Failed", &failed, &failed_ref);
	assert_state(fx, done, "completed");
	assert_state(fx, failed, "failed");

	/* Completed: the entry goes, the folder stays. */
	g_assert_true(axis_downloads_remove(fx->downloads, done, TRUE, &error));
	g_assert_no_error(error);
	call = g_strdup_printf("history:delete:%s:del=1", done_ref);
	g_assert_cmpuint(sab_count(fx->sab, call, FALSE), ==, 1);
	g_assert_false(sab_has(fx->sab, done_ref));
	g_assert_true(g_file_test(done_folder, G_FILE_TEST_IS_DIR));
	g_assert_cmpuint(sab_count(fx->sab, "deleted-files:", TRUE), ==, 0);

	/* Failed: its files go too. */
	g_assert_true(axis_downloads_remove(fx->downloads, failed, TRUE, &error));
	g_assert_no_error(error);
	g_assert_false(sab_has(fx->sab, failed_ref));
	g_assert_false(g_file_test(failed_folder, G_FILE_TEST_EXISTS));
	g_assert_cmpuint(sab_count(fx->sab, "deleted-files:", TRUE), ==, 1);
}

/* Spec (downloaders): a job of one file is reported by its file inside the
 * job's folder (seen from a real SABnzbd): the download completes at that
 * file, mapped onto the volume, and a removal with its files keeps both the
 * file and its folder when the job completed; a failed one's folder goes. */
static void
test_single_file_job_is_reported_by_its_file(Fixture *fx, gconstpointer data)
{
	static const gchar *const names[] = { "Marrowfield.Lanterns.2011-MADEUP", "Bramblewick.Tides.2010-MADEUP" };
	g_autoptr(GError) error = NULL;
	gchar *folders[2];
	gchar *files[2];
	gint64 ids[2];
	guint i;

	add_map(fx);
	sab_set_disk(fx->sab, fx->disk);
	for (i = 0; i < 2; i++) {
		g_autofree gchar *ref = NULL;
		g_autofree gchar *storage = g_strdup_printf("/downloads/complete/axis/%s/%s.mkv", names[i], names[i]);

		ids[i] = add_release(fx, names[i]);
		ref = ref_of(fx, ids[i]);
		folders[i] = g_strdup_printf("%s/downloads/complete/axis/%s", fx->disk, names[i]);
		files[i] = g_strconcat(fx->disk, storage, NULL);
		g_assert_cmpint(g_mkdir_with_parents(folders[i], 0755), ==, 0);
		g_assert_true(g_file_set_contents(files[i], "a film", -1, NULL));
		sab_status(fx->sab, ref, "Downloading", 500);
		poll(fx);
		sab_finish(fx->sab, ref, i == 0 ? "Completed" : "Failed", storage, i == 0 ? NULL : "Repair failed");
		poll(fx);
	}
	/* Completed at the file, as SABnzbd reported it, through the mapping. */
	assert_state(fx, ids[0], "completed");
	{
		g_autofree gchar *sql = g_strdup_printf("SELECT client_path || '|' || rel_path FROM download WHERE id = %"
			G_GINT64_FORMAT, ids[0]);

		assert_q(fx, "/downloads/complete/axis/Marrowfield.Lanterns.2011-MADEUP/Marrowfield.Lanterns.2011-MADEUP.mkv"
			"|downloads/complete/axis/Marrowfield.Lanterns.2011-MADEUP/Marrowfield.Lanterns.2011-MADEUP.mkv", sql);
	}
	/* Removed with its files: the completed job's file and folder stay. */
	g_assert_true(axis_downloads_remove(fx->downloads, ids[0], TRUE, &error));
	g_assert_no_error(error);
	g_assert_true(g_file_test(files[0], G_FILE_TEST_IS_REGULAR));
	g_assert_true(g_file_test(folders[0], G_FILE_TEST_IS_DIR));
	/* The failed one's go. */
	assert_state(fx, ids[1], "failed");
	g_assert_true(axis_downloads_remove(fx->downloads, ids[1], TRUE, &error));
	g_assert_no_error(error);
	g_assert_false(g_file_test(folders[1], G_FILE_TEST_EXISTS));
	for (i = 0; i < 2; i++) {
		g_free(folders[i]);
		g_free(files[i]);
	}
}

static gchar *
category_folder(
	Fixture  *fx,
	GError  **error
){
	gchar *folder = NULL;

	if (!axis_downloads_category_folder(fx->downloads, fx->client_id, &folder, error))
		g_assert_null(folder);
	return folder;
}

/* Spec (downloaders): SABnzbd is asked where it keeps Axis's category, by
 * get_config, as it works it out: a relative folder beneath its folder for
 * finished downloads, an absolute one as it is, a trailing '*' not part of
 * it; no folder of the category's own, or one that cannot be used, is a
 * refusal with the reason. */
static void
test_category_folder(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *folder = NULL;
	g_autofree gchar *section = NULL;
	g_autofree gchar *keyword = NULL;

	/* Relative: beneath complete_dir, asked for by its keyword alone. */
	sab_set_category_dir(fx->sab, "axis", "axis");
	sab_set_complete_dir(fx->sab, "/downloads/complete/");
	folder = category_folder(fx, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(folder, ==, "/downloads/complete/axis");
	g_assert_cmpuint(sab_count(fx->sab, "get_config", FALSE), ==, 2);
	section = sab_last_config(fx->sab, "section");
	keyword = sab_last_config(fx->sab, "keyword");
	g_assert_cmpstr(section, ==, "misc");
	g_assert_cmpstr(keyword, ==, "complete_dir");
	g_clear_pointer(&folder, g_free);

	/* Absolute: as it is (complete_dir is asked for, to tell it is not
	 * that); and so when SABnzbd does not say what complete_dir is. */
	sab_set_category_dir(fx->sab, "axis", "/srv/usenet/axis-films/");
	folder = category_folder(fx, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(folder, ==, "/srv/usenet/axis-films");
	g_assert_cmpuint(sab_count(fx->sab, "get_config", FALSE), ==, 4);
	g_clear_pointer(&folder, g_free);
	sab_set_complete_dir(fx->sab, NULL);
	folder = category_folder(fx, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(folder, ==, "/srv/usenet/axis-films");
	g_clear_pointer(&folder, g_free);
	sab_set_complete_dir(fx->sab, "/downloads/complete/");

	/* "axis*": the jobs' files straight into the folder; it is still the
	 * category's folder (the import then refuses to take it for a job's). */
	sab_set_category_dir(fx->sab, "axis", "axis*");
	folder = category_folder(fx, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(folder, ==, "/downloads/complete/axis");
	g_clear_pointer(&folder, g_free);

	/* No folder of its own: refused, with the reason. */
	sab_set_category_dir(fx->sab, "axis", "");
	folder = category_folder(fx, &error);
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_REFUSED);
	g_assert_nonnull(strstr(error->message, "no folder of its own"));
	g_clear_error(&error);
	sab_set_category_dir(fx->sab, "axis", "*");
	folder = category_folder(fx, &error);
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_REFUSED);
	g_clear_error(&error);
	sab_set_category_dir(fx->sab, "axis", "/downloads/complete");
	folder = category_folder(fx, &error);
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_REFUSED);
	g_assert_nonnull(strstr(error->message, "every finished download"));
	g_clear_error(&error);

	/* A ".." in it: refused, never resolved. */
	sab_set_category_dir(fx->sab, "axis", "../axis");
	folder = category_folder(fx, &error);
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_REFUSED);
	g_assert_nonnull(strstr(error->message, "'..'"));
	g_clear_error(&error);

	/* Relative, and complete_dir not a full path, or not given. */
	sab_set_category_dir(fx->sab, "axis", "axis");
	sab_set_complete_dir(fx->sab, "Downloads/complete");
	folder = category_folder(fx, &error);
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_REFUSED);
	g_assert_nonnull(strstr(error->message, "full path"));
	g_clear_error(&error);
	sab_set_complete_dir(fx->sab, NULL);
	folder = category_folder(fx, &error);
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_UNREADABLE);
	g_clear_error(&error);

	/* No such category at SABnzbd. */
	sab_set_complete_dir(fx->sab, "/downloads/complete");
	{
		static const gchar *const others[] = { "manual", NULL };

		sab_set_categories(fx->sab, others);
		folder = category_folder(fx, &error);
		g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_REFUSED);
		g_assert_nonnull(strstr(error->message, "no category called \"axis\""));
		g_clear_error(&error);
	}

	/* An answer that cannot be read, and a key that no longer works. */
	sab_raw(fx->sab, "get_config", 200, "application/json", "{\"config\": {\"categories\": \"nope\"}}");
	folder = category_folder(fx, &error);
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_UNREADABLE);
	g_clear_error(&error);
	sab_clear_raw(fx->sab, "get_config");
	sab_raw_fixture(fx->sab, "get_config", "wrong-key.json");
	folder = category_folder(fx, &error);
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_AUTH);
	g_clear_error(&error);
	g_assert_cmpuint(sab_bad_requests(fx->sab), ==, 0);
}

/* Reading get_config's answers (the hand-written fixtures; the shape of the
 * categories' list is the one seen from SABnzbd 5.1.3). */
static void
test_parse_config(void)
{
	static const struct
	{
		const gchar *complete;
		const gchar *dir;
		const gchar *folder;	/* NULL: refused */
	} joins[] = {
		{ "/downloads/complete", "axis", "/downloads/complete/axis" },
		{ "/downloads/complete/", "axis/films/", "/downloads/complete/axis/films" },
		{ NULL, "/srv/usenet/axis", "/srv/usenet/axis" },
		{ "relative/complete", "/srv/usenet/axis", "/srv/usenet/axis" },
		{ "/downloads/complete", "axis*", "/downloads/complete/axis" },
		{ "/downloads/complete", " axis ", "/downloads/complete/axis" },
		{ "/downloads/complete", "", NULL },
		{ "/downloads/complete", "*", NULL },
		{ "/downloads/complete", "/downloads/complete", NULL },
		{ "/downloads/complete", "/downloads//complete/", NULL },
		{ "/downloads/complete", "..", NULL },
		{ "/downloads/complete", "axis/../other", NULL },
		{ "/downloads/complete", "./axis", NULL },
		{ "relative/complete", "axis", NULL },
		{ NULL, "axis", NULL },
		{ "/downloads/complete", "/srv/usenet/a\x01b", NULL }
	};
	AxisJsonDoc *doc = doc_of("get-config-categories.json");
	gchar *dir = NULL;
	gchar *complete;
	guint i;

	g_assert_cmpint(axis_sabnzbd_parse_category_dir(axis_json_doc_get_root(doc), "axis", &dir), ==, 1);
	g_assert_cmpstr(dir, ==, "axis");
	g_free(dir);
	g_assert_cmpint(axis_sabnzbd_parse_category_dir(axis_json_doc_get_root(doc), "AXIS", &dir), ==, 1);
	g_free(dir);
	g_assert_cmpint(axis_sabnzbd_parse_category_dir(axis_json_doc_get_root(doc), "manual", &dir), ==, 1);
	g_assert_cmpstr(dir, ==, "/srv/usenet/by-hand");
	g_free(dir);
	g_assert_cmpint(axis_sabnzbd_parse_category_dir(axis_json_doc_get_root(doc), "shared", &dir), ==, 1);
	g_assert_cmpstr(dir, ==, "");
	g_free(dir);
	g_assert_cmpint(axis_sabnzbd_parse_category_dir(axis_json_doc_get_root(doc), "nobody", &dir), ==, 0);
	g_assert_null(dir);
	/* A "dir" that is not text. */
	g_assert_cmpint(axis_sabnzbd_parse_category_dir(axis_json_doc_get_root(doc), "broken", &dir), ==, -1);
	g_assert_null(dir);
	axis_json_doc_free(doc);
	doc = doc_of_text("{\"config\": {}}");
	g_assert_cmpint(axis_sabnzbd_parse_category_dir(axis_json_doc_get_root(doc), "axis", &dir), ==, -1);
	g_assert_null(axis_sabnzbd_parse_complete_dir(axis_json_doc_get_root(doc)));
	axis_json_doc_free(doc);

	doc = doc_of("get-config-complete-dir.json");
	complete = axis_sabnzbd_parse_complete_dir(axis_json_doc_get_root(doc));
	g_assert_cmpstr(complete, ==, "/downloads/complete");
	g_free(complete);
	axis_json_doc_free(doc);

	for (i = 0; i < G_N_ELEMENTS(joins); i++) {
		g_autofree gchar *folder = NULL;
		const gchar *problem = NULL;
		gboolean ok = axis_sabnzbd_category_folder(joins[i].complete, joins[i].dir, &folder, &problem);

		g_test_message("%s + %s", joins[i].complete != NULL ? joins[i].complete : "(none)", joins[i].dir);
		if (joins[i].folder != NULL) {
			g_assert_true(ok);
			g_assert_null(problem);
			g_assert_cmpstr(folder, ==, joins[i].folder);
		} else {
			g_assert_false(ok);
			g_assert_nonnull(problem);
			g_assert_null(folder);
		}
	}
}

/* ── The key (3.2) ──────────────────────────────────────────────── */

/* Task 3.2: the key is only ever in a POST body to the client's own address */
static void
test_key_is_only_in_post_bodies(Fixture *fx, gconstpointer data)
{
	g_autoptr(GPtrArray) requests = NULL;
	g_autoptr(AxisDownloadClientInfo) client = NULL;
	g_autoptr(GError) error = NULL;
	AxisDownloadTestResult result;
	gint64 wrong;
	gint64 id;
	guint i;

	/* A whole life: test, add, poll, remove; and a wrong key's calls. */
	g_assert_true(axis_downloads_test_client(fx->downloads, fx->client_id, &result, &error));
	axis_download_test_result_clear(&result);
	id = add_release(fx, "Some.Film.2020.1080p");
	sab_status(fx->sab, "SABnzbd_nzo_t0001", "Downloading", 500);
	poll(fx);
	g_assert_true(axis_downloads_remove(fx->downloads, id, TRUE, &error));
	wrong = add_client(fx, "Wrong", WRONG_KEY);
	g_assert_true(axis_downloads_test_client(fx->downloads, wrong, &result, &error));
	axis_download_test_result_clear(&result);
	poll_client(fx, wrong);

	requests = axis_test_plex_requests(fx->sab->server);
	g_assert_cmpuint(requests->len, >=, 8);
	for (i = 0; i < requests->len; i++) {
		const gchar *one = g_ptr_array_index(requests, i);
		gsize line = strcspn(one, "\n");
		const gchar *blank = strstr(one, "\n\n");
		g_autofree gchar *head = g_strndup(one, blank != NULL ? (gsize)(blank - one) : strlen(one));
		g_autofree gchar *request_line = g_strndup(one, line);
		gboolean wrong_key_call = strstr(one, WRONG_KEY) != NULL;

		/* One shape of request line: a POST of /api, no query. */
		g_assert_cmpstr(request_line, ==, "POST /api");
		/* Neither key is in the line or in any header; one of them is in the body. */
		g_assert_null(strstr(head, KEY));
		g_assert_null(strstr(head, WRONG_KEY));
		g_assert_nonnull(blank);
		g_assert_true(strstr(blank, KEY) != NULL || wrong_key_call);
		g_assert_nonnull(strstr(head, "Content-Type: "));
	}
	g_assert_cmpuint(sab_bad_requests(fx->sab), ==, 0);
	g_assert_true(axis_test_plex_saw_non_get(fx->sab->server));	/* and POSTs they were */

	/* Nowhere else: not in any table but its own, not in a job row, not in
	 * any log line, not in what a client shows. */
	assert_q(fx, "0", "SELECT count(*) FROM download_client c WHERE to_jsonb(c)::text LIKE '%" KEY "%'");
	assert_q(fx, "0", "SELECT count(*) FROM download d WHERE to_jsonb(d)::text LIKE '%" KEY "%'");
	assert_q(fx, "0", "SELECT count(*) FROM download_client c WHERE to_jsonb(c)::text LIKE '%" WRONG_KEY "%'");
	assert_q(fx, "0", "SELECT count(*) FROM job j WHERE to_jsonb(j)::text LIKE '%" KEY "%'");
	assert_q(fx, "0", "SELECT count(*) FROM job j WHERE to_jsonb(j)::text LIKE '%" WRONG_KEY "%'");
	assert_q(fx, "2", "SELECT count(*) FROM download_client_secret WHERE secret IN ('" KEY "', '" WRONG_KEY "')");
	g_assert_cmpuint(logged("*" KEY "*"), ==, 0);
	g_assert_cmpuint(logged("*" WRONG_KEY "*"), ==, 0);
	g_assert_true(axis_downloads_get_client(fx->downloads, wrong, &client, &error));
	g_assert_null(strstr(client->state_message, WRONG_KEY));
	g_assert_cmpint(client->state, ==, AXIS_DOWNLOAD_CLIENT_REFUSED);
}

/* A redirect is never followed, and the key does not go along. */
static void
test_redirect_is_not_followed(Fixture *fx, gconstpointer data)
{
	Sab *other = sab_new(KEY);
	g_autofree gchar *other_base = axis_test_plex_base(other->server);
	g_autofree gchar *elsewhere = g_strconcat(other_base, "/api", NULL);
	g_autofree gchar *transcript = NULL;
	g_autoptr(AxisDownloadClientInfo) client = NULL;
	g_autoptr(GError) error = NULL;
	AxisDownloadPollReport report;
	AxisDownloadTestResult result;
	gint64 id;

	g_mutex_lock(&fx->sab->lock);
	fx->sab->redirect = g_strdup(elsewhere);
	g_mutex_unlock(&fx->sab->lock);
	sab_set_mode(fx->sab, SAB_REDIRECT);
	report = poll(fx);
	g_assert_false(report.reached);
	g_assert_true(axis_downloads_test_client(fx->downloads, fx->client_id, &result, &error));
	axis_download_test_result_clear(&result);
	id = add_release_at(fx, "Some.Film.2020.1080p", &error) ;
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_assert_cmpint(id, >, 0);
	g_assert_cmpuint(axis_test_plex_n_requests(other->server), ==, 0);
	transcript = axis_test_plex_transcript(other->server);
	g_assert_null(strstr(transcript, KEY));
	g_assert_true(axis_downloads_get_client(fx->downloads, fx->client_id, &client, &error));
	g_assert_cmpint(client->state, ==, AXIS_DOWNLOAD_CLIENT_FAILING);
	g_assert_nonnull(strstr(client->state_message, "redirect"));
	sab_free(other);
}

/* ── Base path (1.1, 3.5) ───────────────────────────────────────── */

/* Spec: Served under a path; a client without one is unchanged */
static void
test_requests_carry_the_base_path(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *base = axis_test_plex_base(fx->sab->server);
	g_autoptr(AxisDownloadClientInfo) client = NULL;
	g_autoptr(GError) error = NULL;
	AxisDownloadTestResult result;
	g_autoptr(GPtrArray) requests = NULL;
	gint64 under;
	gint64 id;
	guint i;

	/* A client with none: every request is "/api", as before. */
	g_assert_true(axis_downloads_test_client(fx->downloads, fx->client_id, &result, &error));
	axis_download_test_result_clear(&result);
	g_assert_cmpuint(sab_bad_requests(fx->sab), ==, 0);
	g_assert_true(axis_downloads_get_client(fx->downloads, fx->client_id, &client, &error));
	g_assert_cmpstr(client->base_path, ==, "");
	g_assert_cmpint(client->state, ==, AXIS_DOWNLOAD_CLIENT_OK);
	g_clear_pointer(&client, axis_download_client_info_free);

	/* Served under /sab (typed with a trailing slash, stored without). */
	sab_set_prefix(fx->sab, "/sab");
	axis_test_plex_reset_requests(fx->sab->server);
	under = add_client_at(fx, "Under a path", base, "/sab/", KEY, AXIS_DOWNLOAD_PRIORITY_NORMAL);
	assert_q(fx, "/sab", "SELECT base_path FROM download_client WHERE name = 'Under a path'");
	g_assert_true(axis_downloads_get_client(fx->downloads, under, &client, &error));
	g_assert_cmpstr(client->base_path, ==, "/sab");
	g_clear_pointer(&client, axis_download_client_info_free);
	g_assert_true(axis_downloads_test_client(fx->downloads, under, &result, &error));
	g_assert_cmpstr(result.version, ==, "4.3.3");
	axis_download_test_result_clear(&result);
	{
		g_autoptr(GBytes) nzb = g_bytes_new_static(NZB_TEXT, strlen(NZB_TEXT));
		AxisDownloadRequest request;

		memset(&request, 0, sizeof(request));
		request.client_id = under;
		request.protocol = AXIS_DOWNLOAD_USENET;
		request.file = nzb;
		request.release_name = "Under.A.Path";
		g_assert_true(axis_downloads_add(fx->downloads, &request, &id, &error));
		g_assert_no_error(error);
	}
	poll_client(fx, under);
	g_assert_true(axis_downloads_remove(fx->downloads, id, TRUE, &error));
	g_assert_cmpuint(sab_bad_requests(fx->sab), ==, 0);
	requests = axis_test_plex_requests(fx->sab->server);
	g_assert_cmpuint(requests->len, >=, 6);
	for (i = 0; i < requests->len; i++) {
		const gchar *one = g_ptr_array_index(requests, i);
		gsize line = strcspn(one, "\n");
		g_autofree gchar *request_line = g_strndup(one, line);

		g_assert_cmpstr(request_line, ==, "POST /sab/api");
	}

	/* The client without one, now asked for "/api" where "/sab/api" is
	 * served: not found, so failing, never "working". */
	g_assert_true(axis_downloads_test_client(fx->downloads, fx->client_id, &result, &error));
	axis_download_test_result_clear(&result);
	g_assert_true(axis_downloads_get_client(fx->downloads, fx->client_id, &client, &error));
	g_assert_cmpint(client->state, ==, AXIS_DOWNLOAD_CLIENT_FAILING);
	g_clear_pointer(&client, axis_download_client_info_free);
}

/* Spec: Unsafe prefix; editing a client's base path */
static void
test_base_path_is_checked_when_saved(Fixture *fx, gconstpointer data)
{
	static const gchar *const refused[] = {
		"/a/../b", "/..", "/a?x=1", "/a#b", "sab", "/a//b", "/a b", "/%2e%2e", "/a/./b", "//evil.test", NULL
	};
	g_autofree gchar *base = axis_test_plex_base(fx->sab->server);
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisDownloadClientInfo) client = NULL;
	AxisDownloadClientSpec spec;
	AxisDownloadTestResult result;
	guint i;

	memset(&spec, 0, sizeof(spec));
	spec.name = "Usenet";
	spec.address = base;
	spec.priority = AXIS_DOWNLOAD_PRIORITY_NORMAL;
	spec.enabled = TRUE;
	for (i = 0; refused[i] != NULL; i++) {
		spec.base_path = refused[i];
		g_assert_false(axis_downloads_save_client(fx->downloads, fx->client_id, &spec, NULL, &error));
		g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID);
		g_assert_nonnull(strstr(error->message, "base path"));
		/* Which field, never the value. */
		if (strlen(refused[i]) > 4)
			g_assert_null(strstr(error->message, refused[i] + 2));
		g_clear_error(&error);
		spec.name = "Another";
		spec.kind = AXIS_DOWNLOAD_SABNZBD_NAME;
		g_assert_false(axis_downloads_save_client(fx->downloads, 0, &spec, NULL, &error));
		g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID);
		g_clear_error(&error);
		spec.name = "Usenet";
		spec.kind = NULL;
	}
	assert_q(fx, "", "SELECT base_path FROM download_client");
	assert_q(fx, "1", "SELECT count(*) FROM download_client");

	/* The database says the same, whoever writes it. */
	axis_test_db_exec(fx->t->db,
		"DO $$ BEGIN"
		" INSERT INTO download_client (name, kind, address, base_path) VALUES ('x1', 'sabnzbd', 'http://h.test:1', '/a/../b');"
		" RAISE EXCEPTION 'accepted'; EXCEPTION WHEN check_violation THEN NULL; END $$");
	axis_test_db_exec(fx->t->db,
		"DO $$ BEGIN"
		" INSERT INTO download_client (name, kind, address, base_path) VALUES ('x2', 'sabnzbd', 'http://h.test:1', '/a?b');"
		" RAISE EXCEPTION 'accepted'; EXCEPTION WHEN check_violation THEN NULL; END $$");
	axis_test_db_exec(fx->t->db,
		"DO $$ BEGIN"
		" INSERT INTO download_client (name, kind, address, base_path) VALUES ('x3', 'sabnzbd', 'http://h.test:1', '/a/');"
		" RAISE EXCEPTION 'accepted'; EXCEPTION WHEN check_violation THEN NULL; END $$");
	axis_test_db_exec(fx->t->db,
		"DO $$ BEGIN"
		" INSERT INTO download_client (name, kind, address, base_path) VALUES ('x4', 'sabnzbd', 'http://h.test:1', '/a/.');"
		" RAISE EXCEPTION 'accepted'; EXCEPTION WHEN check_violation THEN NULL; END $$");
	axis_test_db_exec(fx->t->db,
		"INSERT INTO download_client (name, kind, address, base_path) VALUES ('ok', 'sabnzbd', 'http://h.test:1', '/a/b.c-d_e~f')");
	assert_q(fx, "2", "SELECT count(*) FROM download_client");

	/* Changing it forgets what the last contact found, and the next call uses it. */
	g_assert_true(axis_downloads_test_client(fx->downloads, fx->client_id, &result, &error));
	axis_download_test_result_clear(&result);
	assert_q(fx, "ok", "SELECT state FROM download_client WHERE id = (SELECT min(id) FROM download_client)");
	spec.base_path = "/sab";
	g_assert_true(axis_downloads_save_client(fx->downloads, fx->client_id, &spec, NULL, &error));
	g_assert_no_error(error);
	assert_q(fx, "unchecked", "SELECT state FROM download_client WHERE id = (SELECT min(id) FROM download_client)");
	assert_q(fx, "/sab", "SELECT base_path FROM download_client WHERE id = (SELECT min(id) FROM download_client)");
	/* An edit that leaves it alone keeps it, and so does an empty one clear it. */
	spec.base_path = "";
	g_assert_true(axis_downloads_save_client(fx->downloads, fx->client_id, &spec, NULL, &error));
	assert_q(fx, "", "SELECT base_path FROM download_client WHERE id = (SELECT min(id) FROM download_client)");
	spec.base_path = NULL;
	g_assert_true(axis_downloads_save_client(fx->downloads, fx->client_id, &spec, NULL, &error));
	g_assert_true(axis_downloads_get_client(fx->downloads, fx->client_id, &client, &error));
	g_assert_cmpstr(client->base_path, ==, "");
}

/* ── Sent once (3.6) ────────────────────────────────────────────── */

/* A lost answer: SABnzbd took the release and said nothing; matched by name. */
static void
test_lost_answer_is_matched_by_name(Fixture *fx, gconstpointer data)
{
	AxisDownloadPollReport report;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *ref = NULL;
	gint64 id;

	sab_set_mode(fx->sab, SAB_ADD_HANGS);
	id = add_release_at(fx, "Some.Series.S01E01.720p", &error);
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_UNCERTAIN);
	assert_state(fx, id, "sending");
	sab_set_mode(fx->sab, SAB_NORMAL);
	g_assert_cmpuint(sab_count(fx->sab, "addfile", FALSE), ==, 1);

	axis_test_db_exec(fx->t->db, "UPDATE download SET created_at = now() - interval '1 hour'");
	report = poll(fx);
	g_assert_cmpuint(report.adopted, ==, 1);
	g_assert_cmpuint(report.not_found, ==, 0);
	assert_state(fx, id, "queued");
	ref = ref_of(fx, id);
	g_assert_cmpstr(ref, ==, "SABnzbd_nzo_t0001");
	g_assert_cmpuint(sab_count(fx->sab, "addfile", FALSE), ==, 1);
	g_assert_cmpuint(fx->sab->jobs->len, ==, 1);
}

static void
test_crash_before_sending_is_never_sent(Fixture *fx, gconstpointer data)
{
	AxisDownloadPollReport report;
	g_autoptr(GError) error = NULL;
	gint64 id;

	axis_downloads_set_fault(fx->downloads, AXIS_DOWNLOAD_FAULT_AFTER_RECORD);
	id = add_release_at(fx, "Some.Film.2020.1080p", &error);
	g_assert_nonnull(error);
	assert_state(fx, id, "sending");
	g_assert_cmpuint(sab_count(fx->sab, "addfile", FALSE), ==, 0);

	restart(fx);
	report = poll(fx);
	g_assert_cmpuint(report.not_found, ==, 1);
	assert_state(fx, id, "failed");
	poll(fx);
	g_assert_cmpuint(sab_count(fx->sab, "addfile", FALSE), ==, 0);
	g_assert_cmpuint(fx->sab->jobs->len, ==, 0);
}

static void
test_crash_after_sending_is_adopted(Fixture *fx, gconstpointer data)
{
	AxisDownloadPollReport report;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *ref = NULL;
	gint64 id;

	axis_downloads_set_fault(fx->downloads, AXIS_DOWNLOAD_FAULT_AFTER_SEND);
	id = add_release_at(fx, "Some.Film.2020.1080p", &error);
	g_assert_nonnull(error);
	assert_state(fx, id, "sending");
	assert_q(fx, NULL, "SELECT client_ref FROM download");
	g_assert_cmpuint(sab_count(fx->sab, "addfile", FALSE), ==, 1);

	restart(fx);
	report = poll(fx);
	g_assert_cmpuint(report.adopted, ==, 1);
	g_assert_cmpuint(report.not_found, ==, 0);
	assert_state(fx, id, "queued");
	ref = ref_of(fx, id);
	g_assert_cmpstr(ref, ==, "SABnzbd_nzo_t0001");
	g_assert_cmpuint(sab_count(fx->sab, "addfile", FALSE), ==, 1);
	g_assert_cmpuint(fx->sab->jobs->len, ==, 1);
}

/* An unreachable client at send time: nothing went out, so failed, and never sent. */
static void
test_unreachable_client_at_send_time(Fixture *fx, gconstpointer data)
{
	AxisTestPlex *closed = axis_test_plex_new();
	g_autofree gchar *closed_base = axis_test_plex_base(closed);
	g_autoptr(GBytes) nzb = g_bytes_new_static(NZB_TEXT, strlen(NZB_TEXT));
	g_autoptr(GError) error = NULL;
	AxisDownloadRequest request;
	gint64 client;
	gint64 id = 0;

	axis_test_plex_free(closed);
	client = add_client_at(fx, "Down", closed_base, NULL, KEY, AXIS_DOWNLOAD_PRIORITY_NORMAL);
	memset(&request, 0, sizeof(request));
	request.client_id = client;
	request.protocol = AXIS_DOWNLOAD_USENET;
	request.file = nzb;
	request.release_name = "Some.Film.2020.1080p";
	g_assert_false(axis_downloads_add(fx->downloads, &request, &id, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_UNREACHABLE);
	assert_state(fx, id, "failed");
}

/* An unreachable client changes nothing about its downloads. */
static void
test_down_client_fails_nothing(Fixture *fx, gconstpointer data)
{
	AxisDownloadPollReport report;
	gint64 id = add_release(fx, "Some.Film.2020.1080p");
	g_autofree gchar *ref = ref_of(fx, id);

	sab_status(fx->sab, ref, "Downloading", 500);
	poll(fx);
	sab_set_mode(fx->sab, SAB_DOWN);
	report = poll(fx);
	g_assert_false(report.reached);
	assert_state(fx, id, "downloading");
	assert_q(fx, "0", "SELECT missing_polls FROM download");
	sab_set_mode(fx->sab, SAB_NORMAL);
	report = poll(fx);
	g_assert_true(report.reached);
	assert_state(fx, id, "downloading");
}

/* Spec (downloaders): SABnzbd's folder for Axis's category is Axis's own only
 * when no other category's folder is it or lies inside it, as for the torrent
 * kinds' labels and categories (finding 3 of the stage 2 bug hunt: an arr's
 * category beside Axis's with the same folder made its finished jobs Axis's
 * leftovers).  A category with no folder of its own, or SABnzbd's folder for
 * every finished download, puts its jobs beside Axis's folder: that is fine. */
static void
test_category_folder_shared(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *folder = NULL;

	sab_set_complete_dir(fx->sab, "/downloads/complete/");
	/* Both relative: the same folder beneath complete_dir. */
	sab_set_category_dir(fx->sab, "axis", "movies");
	sab_set_category_dir(fx->sab, "manual", "movies");
	folder = category_folder(fx, &error);
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_REFUSED);
	g_assert_nonnull(strstr(error->message, "\"manual\""));
	g_assert_nonnull(strstr(error->message, "not Axis's category's alone"));
	g_assert_null(folder);
	g_clear_error(&error);
	/* The same folder, written another way (absolute, a slash at its end). */
	sab_set_category_dir(fx->sab, "manual", "/downloads/complete/movies/");
	folder = category_folder(fx, &error);
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_REFUSED);
	g_assert_null(folder);
	g_clear_error(&error);
	/* Another category's folder inside Axis's. */
	sab_set_category_dir(fx->sab, "manual", "/downloads/complete/movies/manual");
	folder = category_folder(fx, &error);
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_REFUSED);
	g_assert_null(folder);
	g_clear_error(&error);
	/* Beside it, or around it: Axis's own. */
	sab_set_category_dir(fx->sab, "manual", "tv");
	folder = category_folder(fx, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(folder, ==, "/downloads/complete/movies");
	g_clear_pointer(&folder, g_free);
	sab_set_category_dir(fx->sab, "manual", "");
	folder = category_folder(fx, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(folder, ==, "/downloads/complete/movies");
	g_clear_pointer(&folder, g_free);
	sab_set_category_dir(fx->sab, "manual", "/downloads/complete");
	folder = category_folder(fx, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(folder, ==, "/downloads/complete/movies");
	g_clear_pointer(&folder, g_free);
	/* One that cannot be worked out: Axis's folder cannot be told its alone. */
	sab_set_category_dir(fx->sab, "manual", "../movies");
	folder = category_folder(fx, &error);
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_REFUSED);
	g_assert_null(folder);
}

/* Spec (downloaders): a list that is not whole never makes a download gone or
 * failed (finding 6 of the stage 2 bug hunt: a job of Axis's queued at
 * SABnzbd beyond the 500 its queue is asked for became gone after three
 * polls, and its title was searched for again).  Once the list is whole
 * again, a download it does not show is counted missing as before. */
static void
test_cut_list_makes_nothing_gone(Fixture *fx, gconstpointer data)
{
	g_autoptr(GString) body = NULL;
	g_autoptr(AxisDownloadClientInfo) client = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *sending_text = NULL;
	AxisDownloadPollReport report;
	gint64 id;
	gint64 sending;
	guint i;

	id = add_release(fx, "Some.Film.2020.1080p");
	assert_state(fx, id, "queued");
	/* A release whose answer was lost, long enough ago to be reconciled. */
	axis_test_db_exec(fx->t->db, "INSERT INTO download (client_id, client_name, protocol, source, release_name, state,"
		" created_at) SELECT client_id, client_name, 'usenet', 'file', 'Lost.Answer.2019.720p', 'sending',"
		" now() - interval '1 hour' FROM download LIMIT 1");
	sending_text = axis_test_db_query(fx->t->db, "SELECT id FROM download WHERE state = 'sending'");
	sending = g_ascii_strtoll(sending_text, NULL, 10);
	/* SABnzbd's queue holds 500 other jobs of Axis's category before them,
	 * and answers the 500 asked for. */
	body = g_string_new("{\"queue\": {\"status\": \"Downloading\", \"slots\": [");
	for (i = 0; i < AXIS_DOWNLOAD_SABNZBD_LIST_LIMIT; i++)
		g_string_append_printf(body, "%s{\"nzo_id\": \"SABnzbd_nzo_x%04u\", \"filename\": \"Other.Job.%04u\", \"cat\": "
			"\"axis\", \"status\": \"Queued\", \"mb\": \"1000.0\", \"mbleft\": \"1000.0\", \"percentage\": \"0\"}",
			i > 0 ? ", " : "", i, i);
	g_string_append(body, "]}}");
	sab_raw(fx->sab, "queue", 200, "application/json", body->str);
	for (i = 0; i < 4; i++) {
		report = poll(fx);
		g_assert_true(report.partial);
		g_assert_cmpuint(report.gone, ==, 0);
		g_assert_cmpuint(report.not_found, ==, 0);
	}
	assert_state(fx, id, "queued");
	assert_state(fx, sending, "sending");
	assert_q(fx, "0", "SELECT missing_polls FROM download WHERE state = 'queued'");
	/* The client's page says why. */
	g_assert_true(axis_downloads_get_client(fx->downloads, fx->client_id, &client, &error));
	g_assert_nonnull(client->problem);
	g_assert_nonnull(strstr(client->problem, "not whole"));

	/* Whole again, without them: missing, then gone, as before. */
	sab_raw(fx->sab, "queue", 200, "application/json", "{\"queue\": {\"status\": \"Idle\", \"slots\": []}}");
	for (i = 0; i < 4; i++)
		poll(fx);
	assert_state(fx, id, "gone");
	assert_state(fx, sending, "failed");
}

/* Spec (downloaders): a list holding an entry of Axis's category that cannot
 * be read is not whole (a suspicion of the stage 2 bug hunt, confirmed by
 * this test before the fix: SABnzbd's slots that could not be read were left
 * out silently, and the list taken for whole); tracking then never takes a
 * download it does not show for gone. */
static void
test_unreadable_slot_not_whole(Fixture *fx, gconstpointer data)
{
	g_autoptr(GPtrArray) items = NULL;
	g_autoptr(GError) error = NULL;
	guint unreadable = 0;
	gint64 id;
	guint i;

	id = add_release(fx, "Some.Film.2020.1080p");
	assert_state(fx, id, "queued");
	/* Its slot cannot be read (an identifier SABnzbd does not give). */
	sab_raw(fx->sab, "queue", 200, "application/json", "{\"queue\": {\"status\": \"Downloading\", \"slots\": ["
		"{\"nzo_id\": \"not one of SABnzbd's\", \"filename\": \"Some.Film.2020.1080p\", \"cat\": \"axis\", "
		"\"status\": \"Queued\", \"mb\": \"1000.0\", \"mbleft\": \"1000.0\", \"percentage\": \"0\"}]}}");
	g_assert_true(axis_downloads_client_items(fx->downloads, fx->client_id, &items, &unreadable, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(unreadable, >, 0);
	for (i = 0; i < 4; i++)
		poll(fx);
	assert_state(fx, id, "queued");
}

/* Review of the fixes, items 1 and 2, through SABnzbd's own answers: what it
 * has is asked of it whole, every category (another program's jobs are still
 * something at their paths) and every page of its lists, not the first 500
 * jobs of Axis's category; and each name is kept exactly as SABnzbd gave it,
 * for comparing with names on disk. */
static void
test_everything_listed(Fixture *fx, gconstpointer data)
{
	g_autoptr(GPtrArray) items = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *why = NULL;
	g_autofree gchar *cat = NULL;
	const AxisDownloadItem *spaced = NULL;
	guint unreadable = 99;
	guint other = 0;
	guint i;

	sab_put(fx->sab, "SABnzbd_nzo_spaced", "Double  Space  Name ", "axis", "Downloading", FALSE, 1000.0, 500.0, NULL,
		NULL);
	/* More finished jobs of another program's than one page holds. */
	for (i = 0; i < 2 * AXIS_DOWNLOAD_SABNZBD_LIST_LIMIT + 7; i++) {
		g_autofree gchar *id = g_strdup_printf("SABnzbd_nzo_tv%04u", i);
		g_autofree gchar *name = g_strdup_printf("Their.Show.S01E%02u", i % 90);
		g_autofree gchar *storage = g_strdup_printf("/downloads/complete/tv/Their.Show.%u", i);

		sab_put(fx->sab, id, name, "tv", "Completed", TRUE, 100.0, 0.0, storage, NULL);
	}
	g_assert_true(axis_downloads_client_items_full(fx->downloads, fx->client_id, &items, &unreadable, &why, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(unreadable, ==, 0);
	g_assert_null(why);
	g_assert_cmpuint(items->len, ==, 2 * AXIS_DOWNLOAD_SABNZBD_LIST_LIMIT + 8);
	/* No category asked for. */
	cat = sab_last_query(fx->sab, "cat");
	g_assert_null(cat);
	for (i = 0; i < items->len; i++) {
		const AxisDownloadItem *item = g_ptr_array_index(items, i);

		if (g_strcmp0(item->client_ref, "SABnzbd_nzo_spaced") == 0)
			spaced = item;
		if (g_strcmp0(item->category, "tv") == 0)
			other++;
	}
	g_assert_cmpuint(other, ==, 2 * AXIS_DOWNLOAD_SABNZBD_LIST_LIMIT + 7);
	g_assert_nonnull(spaced);
	g_assert_cmpstr(spaced->name, ==, "Double Space Name");
	g_assert_cmpstr(spaced->own_name, ==, "Double  Space  Name ");
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/download/sabnzbd/parse/replies", test_parse_replies);
	g_test_add_func("/download/sabnzbd/parse/queue", test_parse_queue);
	g_test_add_func("/download/sabnzbd/parse/history", test_parse_history);
	g_test_add_func("/download/sabnzbd/parse/broken", test_parse_broken_answers);
	g_test_add_func("/download/sabnzbd/parse/capped", test_parse_capped);
	g_test_add_func("/download/sabnzbd/parse/figures", test_parse_figures);
	g_test_add_func("/download/sabnzbd/parse/config", test_parse_config);
	g_test_add_func("/download/sabnzbd/base-path/rules", test_base_path_rules);
#define ADD(path, func) AXIS_TEST_ADD_DB_CASE("/download/sabnzbd/" path, Fixture, NULL, func)
	ADD("client/test", test_client_test);
	ADD("client/category-missing", test_client_test_category_missing);
	ADD("client/wrong-key", test_client_wrong_key_is_not_unreachable);
	ADD("client/old-plain-text-wrong-key", test_client_old_plain_text_wrong_key);
	ADD("client/hostname-check", test_client_hostname_check_is_not_a_refused_key);
	ADD("client/other-403s", test_other_403s_stay_a_refused_key);
	ADD("add/hostname-check", test_add_hostname_check_is_not_a_refused_key);
	ADD("add/upload", test_add_uploads_the_release);
	ADD("add/priorities", test_add_priorities);
	ADD("add/file-name", test_add_file_name_is_made_here);
	ADD("add/boundary", test_add_boundary_is_not_in_the_release);
	ADD("add/refused-in-200", test_add_refused_inside_a_200);
	ADD("add/one-identifier", test_add_needs_exactly_one_identifier);
	ADD("add/wrong-key", test_add_with_a_wrong_key);
	ADD("add/other-protocols", test_add_refuses_other_protocols);
	ADD("list/other-categories", test_other_categories_are_never_touched);
	ADD("list/fixtures", test_fixture_answers_through_the_layer);
	ADD("list/unreadable", test_unreadable_lists_change_nothing);
	ADD("list/unreadable-slot", test_unreadable_slot_not_whole);
	ADD("list/everything", test_everything_listed);
	ADD("track/post-processing", test_post_processing_then_completed);
	ADD("track/failed", test_failed_with_the_clients_reason);
	ADD("track/unknown-status", test_unknown_status_keeps_the_last_state);
	ADD("track/unknown-status-fixtures", test_unknown_status_fixtures);
	ADD("track/client-down", test_down_client_fails_nothing);
	ADD("remove/where-it-is", test_remove_from_the_queue_or_the_history);
	ADD("remove/no-memory", test_remove_with_no_memory_tries_both);
	ADD("remove/refusal-and-key", test_remove_after_a_refusal_and_with_a_wrong_key);
	ADD("remove/completed-keeps-folder", test_remove_keeps_a_completed_jobs_folder);
	ADD("remove/single-file-job", test_single_file_job_is_reported_by_its_file);
	ADD("folder/category", test_category_folder);
	ADD("folder/category-shared", test_category_folder_shared);
	ADD("track/cut-list-makes-nothing-gone", test_cut_list_makes_nothing_gone);
	ADD("key/post-bodies-only", test_key_is_only_in_post_bodies);
	ADD("key/redirect", test_redirect_is_not_followed);
	ADD("base-path/requests", test_requests_carry_the_base_path);
	ADD("base-path/saved", test_base_path_is_checked_when_saved);
	ADD("once/lost-answer", test_lost_answer_is_matched_by_name);
	ADD("once/crash-before-send", test_crash_before_sending_is_never_sent);
	ADD("once/crash-after-send", test_crash_after_sending_is_adopted);
	ADD("once/unreachable", test_unreachable_client_at_send_time);
#undef ADD
	return g_test_run();
}
