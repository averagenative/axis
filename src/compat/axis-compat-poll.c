/*
 * axis-compat-poll.c - one look at the sessions in progress: recorded once
 * each, matched to Axis files, with their evidence
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "compat/axis-compat-private.h"

#include <string.h>

#include "compat/axis-compat-evidence.h"
#include "jobs/axis-job-error.h"
#include "library/axis-library-name.h"

/* A poll fetches the detail of at most this many new items. */
#define MAX_ITEM_LOOKUPS 16

/* The retention runs at most this often from the poll. */
#define RETENTION_EVERY_SECONDS (10 * 60)

/* ── Facts about a matched file ─────────────────────────────────── */

typedef struct
{
	gint64              file_id;	/* 0: not matched */
	AxisCompatFileFacts facts;
	gboolean            have_facts;
	gchar              *container;
	gchar              *video_codec;
	gchar              *video_profile;
	gchar              *res_class;
	gchar              *audio_codec;
} Match;

static void
match_clear(Match *match)
{
	g_free(match->container);
	g_free(match->video_codec);
	g_free(match->video_profile);
	g_free(match->res_class);
	g_free(match->audio_codec);
	memset(match, 0, sizeof(*match));
}

static const gchar *
text_or_null(
	AxisDbResult *result,
	guint         row,
	guint         column
){
	return axis_db_result_is_null(result, row, column) ? NULL : axis_db_result_get_text(result, row, column);
}

/* What Axis's probe found in file @file_id, for the classification. */
static gboolean
load_facts(
	AxisDbConn *conn,
	Match      *match,
	GError    **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT, match->file_id);
	const gchar *params[1];
	const gchar *hdr;
	gint64 value = 0;
	gboolean flag = FALSE;
	gint64 video_streams = 0;
	gint64 audio_streams = 0;

	params[0] = id;
	if (!axis_db_conn_exec(conn,
	        "SELECT m.container, m.video_codec, m.bit_depth, m.resolution_class, m.hdr_format, m.hdr_uncertain,"
	        " m.audio_codec, m.audio_channels, m.bitrate, m.video_streams, m.audio_streams,"
	        " (SELECT s.profile FROM media_stream s WHERE s.file_id = m.file_id AND s.kind = 'video'"
	        "  AND NOT s.attached_pic ORDER BY s.idx LIMIT 1)"
	        " FROM media_info m WHERE m.file_id = $1::bigint AND m.status = 'ok'",
	        1, params, &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) == 0)
		return TRUE;
	axis_db_result_get_int64(result, 0, 9, &video_streams);
	axis_db_result_get_int64(result, 0, 10, &audio_streams);
	match->have_facts = TRUE;
	match->container = g_strdup(text_or_null(result, 0, 0));
	match->facts.container = match->container;
	match->facts.hdr = -1;
	if (axis_db_result_get_int64(result, 0, 8, &value) && value > 0)
		match->facts.bitrate_kbps = value / 1000;
	if (video_streams == 1) {
		match->video_codec = g_strdup(text_or_null(result, 0, 1));
		match->facts.video_codec = match->video_codec;
		match->video_profile = g_strdup(text_or_null(result, 0, 11));
		match->facts.video_profile = match->video_profile;
		if (axis_db_result_get_int64(result, 0, 2, &value))
			match->facts.bit_depth = (gint)value;
		match->res_class = g_strdup(text_or_null(result, 0, 3));
		match->facts.res_class = match->res_class;
		hdr = text_or_null(result, 0, 4);
		if (axis_db_result_get_boolean(result, 0, 5, &flag) && flag)
			match->facts.hdr = -1;
		else if (g_strcmp0(hdr, "sdr") == 0)
			match->facts.hdr = 0;
		else if (hdr != NULL)
			match->facts.hdr = 1;
	}
	if (audio_streams == 1) {
		match->audio_codec = g_strdup(text_or_null(result, 0, 6));
		match->facts.audio_codec = match->audio_codec;
		if (axis_db_result_get_int64(result, 0, 7, &value))
			match->facts.audio_channels = (gint)value;
	}
	return TRUE;
}

/* Matches a path the server states to an indexed file, through the path
 * map; @match->file_id stays 0 when there is none. */
static gboolean
match_path(
	AxisDbConn  *conn,
	GPtrArray   *map,
	GPtrArray   *roots,
	const gchar *server_path,
	Match       *match,
	GError     **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *mapped = NULL;
	g_autofree gchar *rel = NULL;
	g_autofree gchar *root_id = NULL;
	const AxisCompatRootRef *root;
	GString *stored;
	const gchar *params[2];
	gint64 id = 0;

	match->file_id = 0;
	if (server_path == NULL)
		return TRUE;
	mapped = axis_compat_path_map_apply(map, server_path);
	root = axis_compat_root_for_path(roots, mapped, &rel);
	if (root == NULL || rel == NULL)
		return TRUE;
	stored = g_string_new(NULL);
	axis_library_name_encode(stored, rel, strlen(rel));
	root_id = g_strdup_printf("%" G_GINT64_FORMAT, root->id);
	params[0] = root_id;
	params[1] = stored->str;
	if (!axis_db_conn_exec(conn,
	        "SELECT f.id FROM library_file f WHERE f.root_id = $1::bigint AND f.path_hash = library_path_hash($2::text)"
	        " AND f.rel_path = $2::text AND f.state <> 'missing'",
	        2, params, &result, error)) {
		g_string_free(stored, TRUE);
		return FALSE;
	}
	g_string_free(stored, TRUE);
	if (axis_db_result_get_n_rows(result) == 1 && axis_db_result_get_int64(result, 0, 0, &id))
		match->file_id = id;
	return TRUE;
}

/* ── Libraries, for a server whose sessions do not say ──────────── */

typedef struct
{
	gchar     *key;
	GPtrArray *folders;	/* normalised paths, as the server states them */
} LibraryFolders;

static void
library_folders_free(LibraryFolders *library)
{
	g_free(library->key);
	g_ptr_array_unref(library->folders);
	g_free(library);
}

static gboolean
load_library_folders(
	AxisDbConn  *conn,
	const gchar *kind,
	GPtrArray  **libraries,
	GError     **error
){
	g_autoptr(AxisDbResult) result = NULL;
	const gchar *params[1];
	guint rows;
	guint i;

	params[0] = kind;
	*libraries = g_ptr_array_new_with_free_func((GDestroyNotify)library_folders_free);
	if (!axis_db_conn_exec(conn,
	        "SELECT l.key, f.folder FROM compat_library l CROSS JOIN LATERAL unnest(l.folders) AS f(folder)"
	        " WHERE l.server_kind = $1::text ORDER BY l.key", 1, params, &result, error))
		return FALSE;
	rows = axis_db_result_get_n_rows(result);
	for (i = 0; i < rows; i++) {
		const gchar *key = axis_db_result_get_text(result, i, 0);
		g_autofree gchar *folder = axis_compat_path_normalize(axis_db_result_get_text(result, i, 1), NULL);
		LibraryFolders *library = (*libraries)->len > 0 ?
			g_ptr_array_index(*libraries, (*libraries)->len - 1) : NULL;

		if (folder == NULL)
			continue;
		if (library == NULL || strcmp(library->key, key) != 0) {
			library = g_new0(LibraryFolders, 1);
			library->key = g_strdup(key);
			library->folders = g_ptr_array_new_with_free_func(g_free);
			g_ptr_array_add(*libraries, library);
		}
		g_ptr_array_add(library->folders, g_steal_pointer(&folder));
	}
	return TRUE;
}

/* The key of the library @path is in: the one whose folder is the longest
 * that contains it, but only when exactly one library has that folder.  A
 * path in no library, or in two at once, is in none that can be named. */
static const gchar *
library_for_path(
	GPtrArray   *libraries,
	const gchar *path
){
	g_autofree gchar *clean = path != NULL ? axis_compat_path_normalize(path, NULL) : NULL;
	const gchar *best = NULL;
	gsize best_length = 0;
	gboolean tie = FALSE;
	guint i;

	if (clean == NULL || libraries == NULL)
		return NULL;
	for (i = 0; i < libraries->len; i++) {
		const LibraryFolders *library = g_ptr_array_index(libraries, i);
		guint j;

		for (j = 0; j < library->folders->len; j++) {
			const gchar *folder = g_ptr_array_index(library->folders, j);
			gsize length = strlen(folder);
			gboolean inside = strcmp(folder, "/") == 0 || strcmp(clean, folder) == 0 ||
				(strncmp(clean, folder, length) == 0 && clean[length] == '/');

			if (!inside)
				continue;
			if (length > best_length) {
				best = library->key;
				best_length = length;
				tie = FALSE;
			} else if (length == best_length && g_strcmp0(best, library->key) != 0) {
				tie = TRUE;
			}
		}
	}
	return tie ? NULL : best;
}

/* ── Writing ────────────────────────────────────────────────────── */

static gchar *
int_text(gint64 value)
{
	return g_strdup_printf("%" G_GINT64_FORMAT, value);
}

static gboolean
store_evidence(
	AxisDbConn *conn,
	gint64      session_id,
	GPtrArray  *evidence,
	GError    **error
){
	guint i;

	for (i = 0; i < evidence->len; i++) {
		const AxisCompatEvidence *ev = g_ptr_array_index(evidence, i);
		g_autofree gchar *sid = int_text(session_id);
		g_autofree gchar *kind = int_text(ev->kind);
		g_autofree gchar *depth = int_text(ev->bit_depth);
		g_autofree gchar *hdr = int_text(ev->hdr);
		g_autofree gchar *channels = int_text(ev->channels);
		const gchar *params[13];

		params[0] = sid;
		params[1] = kind;
		params[2] = ev->codec;
		params[3] = ev->profile;
		params[4] = depth;
		params[5] = ev->res_class;
		params[6] = hdr;
		params[7] = channels;
		params[8] = ev->container;
		params[9] = axis_compat_effect_to_string(ev->effect);
		params[10] = ev->out_codec;
		params[11] = ev->stated_reason;
		params[12] = axis_compat_property_to_string(ev->property);
		/* A later look that learns nothing does not undo an earlier one. */
		if (!axis_db_conn_exec(conn,
		        "INSERT INTO compat_evidence (session_id, kind, codec, profile, bit_depth, res_class, hdr, channels,"
		        " container, effect, out_codec, stated_reason, failed_property) VALUES ($1::bigint, $2::smallint,"
		        " $3::text, $4::text, $5::integer, $6::text, $7::smallint, $8::integer, $9::text, $10::text,"
		        " $11::text, $12::text, $13::text)"
		        " ON CONFLICT (session_id, kind, failed_property) DO UPDATE SET codec = EXCLUDED.codec,"
		        " profile = EXCLUDED.profile, bit_depth = EXCLUDED.bit_depth, res_class = EXCLUDED.res_class,"
		        " hdr = EXCLUDED.hdr, channels = EXCLUDED.channels, container = EXCLUDED.container,"
		        " effect = EXCLUDED.effect, out_codec = EXCLUDED.out_codec, stated_reason = EXCLUDED.stated_reason"
		        " WHERE EXCLUDED.effect <> 'unknown'",
		        13, params, NULL, error))
			return FALSE;
	}
	return TRUE;
}

typedef struct
{
	AxisCompat           *self;
	const AxisCompatKind *kind;
	GPtrArray            *map;
	GPtrArray            *roots;
	GPtrArray            *libraries;	/* LibraryFolders, for a kind whose sessions do not name theirs */
	gboolean              names;
	gchar                *server_id;
} Writer;

/* Records one session (or notes that it was seen again). */
static gboolean
record_session(
	Writer                  *writer,
	AxisDbConn              *conn,
	const AxisCompatSession *s,
	const gchar             *item_file,
	AxisCompatPollSummary   *summary,
	GError                 **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(GPtrArray) evidence = NULL;
	g_autofree gchar *bandwidth = NULL;
	Match match;
	const gchar *params[13];
	const gchar *library_key = s->library_key;
	gboolean inserted = FALSE;
	gint64 session_id = 0;
	gboolean remote;
	gboolean ok = FALSE;

	memset(&match, 0, sizeof(match));
	remote = !(s->player_local == AXIS_COMPAT_YES || g_strcmp0(s->location, "lan") == 0);
	bandwidth = s->bandwidth_kbps >= 0 ? int_text(s->bandwidth_kbps) : NULL;
	if (library_key == NULL && writer->kind->library_from_path)
		library_key = library_for_path(writer->libraries, s->file != NULL ? s->file : item_file);

	params[0] = s->device_client_id;
	params[1] = s->device_name;
	params[2] = s->platform;
	params[3] = s->product;
	params[4] = writer->kind->name;
	if (!axis_db_conn_exec(conn,
	        "INSERT INTO compat_device (client_id, name, platform, product, server_kind) VALUES ($1::text, $2::text,"
	        " $3::text, $4::text, $5::text) ON CONFLICT (server_kind, client_id) DO UPDATE SET"
	        " name = coalesce(EXCLUDED.name, compat_device.name),"
	        " platform = coalesce(EXCLUDED.platform, compat_device.platform),"
	        " product = coalesce(EXCLUDED.product, compat_device.product), last_seen = now()",
	        5, params, NULL, error))
		return FALSE;

	params[0] = writer->server_id;
	params[1] = s->session_key;
	params[2] = s->rating_key;
	params[3] = s->device_client_id;
	params[4] = library_key;
	params[5] = s->item_type;
	params[6] = remote ? "true" : "false";
	params[7] = bandwidth;
	params[8] = s->account_id;
	params[9] = writer->names ? s->account_name : NULL;
	params[10] = writer->kind->name;
	if (!axis_db_conn_exec(conn,
	        "INSERT INTO compat_session (server_id, session_key, rating_key, device_id, library_key, item_type, remote,"
	        " bandwidth_kbps, account_id, account_name, server_kind) VALUES ($1::text, $2::text, $3::text, $4::text,"
	        " $5::text, $6::text, $7::boolean, $8::integer, $9::text, $10::text, $11::text)"
	        " ON CONFLICT (server_kind, server_id, session_key, rating_key) DO UPDATE SET last_seen = now(),"
	        " polls = compat_session.polls + 1, remote = EXCLUDED.remote,"
	        " bandwidth_kbps = coalesce(EXCLUDED.bandwidth_kbps, compat_session.bandwidth_kbps),"
	        " library_key = coalesce(EXCLUDED.library_key, compat_session.library_key)"
	        " RETURNING id, (xmax = 0) AS inserted",
	        11, params, &result, error))
		return FALSE;
	axis_db_result_get_int64(result, 0, 0, &session_id);
	axis_db_result_get_boolean(result, 0, 1, &inserted);
	g_clear_pointer(&result, axis_db_result_free);

	/* Which file was played: the one already matched, else look again. */
	{
		g_autofree gchar *sid = int_text(session_id);
		const gchar *p[1];

		p[0] = sid;
		if (!axis_db_conn_exec(conn, "SELECT file_id FROM compat_session WHERE id = $1::bigint", 1, p, &result, error))
			return FALSE;
		if (!axis_db_result_is_null(result, 0, 0))
			axis_db_result_get_int64(result, 0, 0, &match.file_id);
		g_clear_pointer(&result, axis_db_result_free);
		if (match.file_id == 0) {
			if (!match_path(conn, writer->map, writer->roots, s->file != NULL ? s->file : item_file, &match, error))
				goto out;
			if (match.file_id != 0) {
				g_autofree gchar *fid = int_text(match.file_id);
				const gchar *q[2];

				q[0] = sid;
				q[1] = fid;
				if (!axis_db_conn_exec(conn, "UPDATE compat_session SET file_id = $2::bigint WHERE id = $1::bigint",
				        2, q, NULL, error))
					goto out;
				if (inserted)
					summary->matched++;
			}
		}
	}
	if (match.file_id != 0 && !load_facts(conn, &match, error))
		goto out;

	evidence = axis_compat_classify_session(s, match.have_facts ? &match.facts : NULL);
	if (!store_evidence(conn, session_id, evidence, error))
		goto out;
	if (inserted)
		summary->recorded++;
	else
		summary->updated++;
	ok = TRUE;
out:
	match_clear(&match);
	return ok;
}

/* ── The poll ───────────────────────────────────────────────────── */

typedef struct
{
	gboolean due;
	gdouble  libraries_age;
	gchar   *server_id;
} PollState;

static void
poll_state_clear(PollState *state)
{
	g_clear_pointer(&state->server_id, g_free);
}

static gboolean
load_state(
	AxisCompat           *self,
	const AxisCompatKind *kind,
	PollState            *state,
	GError              **error
){
	g_autoptr(AxisDbResult) result = NULL;
	const gchar *params[1];

	memset(state, 0, sizeof(*state));
	params[0] = kind->name;
	if (!axis_db_pool_exec(self->pool,
	        "SELECT (next_poll_at IS NULL OR next_poll_at <= now()),"
	        " coalesce(extract(epoch FROM now() - libraries_at), -1)::double precision, server_id"
	        " FROM compat_state WHERE server_kind = $1::text", 1, params, &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) == 0) {
		/* A kind's row is made when it is first used. */
		state->due = TRUE;
		state->libraries_age = -1;
		return TRUE;
	}
	axis_db_result_get_boolean(result, 0, 0, &state->due);
	state->libraries_age = g_ascii_strtod(axis_db_result_get_text(result, 0, 1), NULL);
	state->server_id = g_strdup(text_or_null(result, 0, 2));
	return TRUE;
}

static gboolean
session_is_recordable(const AxisCompatSession *s)
{
	return s->session_key != NULL && s->rating_key != NULL && s->device_client_id != NULL;
}

/* Whether a session is already recorded, on a pooled connection. */
static gboolean
session_known(
	AxisCompat              *self,
	const gchar             *kind,
	const gchar             *server_id,
	const AxisCompatSession *s,
	gboolean                *known,
	GError                 **error
){
	g_autoptr(AxisDbResult) result = NULL;
	const gchar *params[4];

	params[0] = server_id;
	params[1] = s->session_key;
	params[2] = s->rating_key;
	params[3] = kind;
	if (!axis_db_pool_exec(self->pool,
	        "SELECT 1 FROM compat_session WHERE server_id = $1::text AND session_key = $2::text AND rating_key = $3::text"
	        " AND server_kind = $4::text",
	        4, params, &result, error))
		return FALSE;
	*known = axis_db_result_get_n_rows(result) > 0;
	return TRUE;
}

static gboolean
do_poll(
	AxisCompat                 *self,
	const AxisCompatConnection *connection,
	const PollState            *state,
	GCancellable               *cancellable,
	AxisCompatPollSummary      *summary,
	GError                    **error
){
	const AxisCompatKind *kind = connection->kind;
	AxisCompatIdentity identity;
	g_autoptr(GPtrArray) sessions = NULL;
	g_autoptr(GPtrArray) item_files = NULL;
	g_autofree gchar *playing = NULL;
	g_autofree gchar *poll_interval = NULL;
	g_autofree gchar *idle_interval = NULL;
	Writer writer;
	AxisDbConn *conn = NULL;
	const gchar *params[4];
	guint lookups = 0;
	gboolean table_full = FALSE;
	gboolean ok = FALSE;
	guint i;

	memset(&writer, 0, sizeof(writer));
	if (!axis_compat_fetch_identity(self, connection, cancellable, &identity, error))
		return FALSE;

	if (state->libraries_age < 0 || state->libraries_age > AXIS_COMPAT_LIBRARIES_REFRESH_SECONDS ||
	    g_strcmp0(state->server_id, identity.machine_id) != 0) {
		g_autoptr(GError) local = NULL;
		AxisDbConn *c = axis_db_pool_acquire(self->pool, error);

		if (c == NULL) {
			axis_compat_identity_clear(&identity);
			return FALSE;
		}
		/* The server is noted first: a new one clears the old records. */
		if (!axis_db_conn_begin(c, error) || !axis_compat_adopt_server(c, kind->name, &identity, error) ||
		    !axis_db_conn_commit(c, error)) {
			axis_db_conn_rollback(c, NULL);
			axis_db_pool_release(self->pool, c);
			axis_compat_identity_clear(&identity);
			return FALSE;
		}
		axis_db_pool_release(self->pool, c);
		/* The libraries are a convenience: the evidence does not wait for them. */
		if (!axis_compat_refresh_libraries(self, connection, cancellable, &local)) {
			if (g_error_matches(local, AXIS_JOB_ERROR, AXIS_JOB_ERROR_CANCELLED)) {
				g_propagate_error(error, g_steal_pointer(&local));
				axis_compat_identity_clear(&identity);
				return FALSE;
			}
			g_message("compat: the libraries could not be read: %s", local->message);
		}
	}

	if (!kind->fetch_sessions(self, connection, cancellable, &sessions, error)) {
		axis_compat_identity_clear(&identity);
		return FALSE;
	}
	summary->playing = sessions->len;

	/* The file of a new item, when the session does not say. */
	item_files = g_ptr_array_new_with_free_func(g_free);
	g_ptr_array_set_size(item_files, sessions->len);
	for (i = 0; i < sessions->len && kind->fetch_item_file != NULL; i++) {
		const AxisCompatSession *s = g_ptr_array_index(sessions, i);
		gboolean known = FALSE;

		if (!session_is_recordable(s) || s->file != NULL || lookups >= MAX_ITEM_LOOKUPS)
			continue;
		if (!session_known(self, kind->name, identity.machine_id, s, &known, error))
			goto out;
		if (known)
			continue;
		{
			g_autoptr(GError) local = NULL;
			gchar *file = NULL;

			lookups++;
			if (!kind->fetch_item_file(self, connection, cancellable, s->rating_key, &file, &local)) {
				if (g_error_matches(local, AXIS_JOB_ERROR, AXIS_JOB_ERROR_CANCELLED)) {
					g_propagate_error(error, g_steal_pointer(&local));
					goto out;
				}
				continue;
			}
			item_files->pdata[i] = file;
		}
	}

	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		goto out;
	writer.self = self;
	writer.kind = kind;
	writer.server_id = identity.machine_id;
	writer.names = self->settings != NULL &&
		axis_settings_get_boolean(self->settings, AXIS_COMPAT_SETTING_ACCOUNT_NAMES);
	if (!axis_compat_load_path_map(conn, kind->name, &writer.map, error) ||
	    !axis_compat_load_roots(conn, &writer.roots, error))
		goto out;
	if (kind->library_from_path && !load_library_folders(conn, kind->name, &writer.libraries, error))
		goto out;
	{
		g_autoptr(AxisDbResult) counted = NULL;
		g_autofree gchar *limit = int_text(self->tuning.max_session_rows);
		const gchar *count_params[1];

		count_params[0] = limit;
		if (!axis_db_conn_exec(conn,
		        "SELECT count(*) >= $1::bigint FROM (SELECT 1 FROM compat_session LIMIT $1::bigint) x", 1, count_params,
		        &counted, error))
			goto out;
		axis_db_result_get_boolean(counted, 0, 0, &table_full);
	}
	if (!axis_db_conn_begin(conn, error))
		goto out;
	for (i = 0; i < sessions->len; i++) {
		const AxisCompatSession *s = g_ptr_array_index(sessions, i);

		if (!session_is_recordable(s)) {
			summary->skipped++;
			continue;
		}
		if (table_full) {
			gboolean known = FALSE;

			if (!session_known(self, kind->name, identity.machine_id, s, &known, error))
				goto out;
			if (!known) {
				summary->skipped++;
				continue;
			}
		}
		if (!record_session(&writer, conn, s, g_ptr_array_index(item_files, i), summary, error))
			goto out;
	}

	playing = int_text(summary->playing);
	poll_interval = int_text(self->settings != NULL ?
		axis_settings_get_duration(self->settings, AXIS_COMPAT_SETTING_POLL_INTERVAL) : 30);
	idle_interval = int_text(self->settings != NULL ?
		axis_settings_get_duration(self->settings, AXIS_COMPAT_SETTING_IDLE_INTERVAL) : 300);
	params[0] = playing;
	params[1] = poll_interval;
	params[2] = idle_interval;
	params[3] = kind->name;
	if (!axis_compat_ensure_state(conn, kind->name, error) ||
	    !axis_db_conn_exec(conn,
	        "UPDATE compat_state SET last_poll_at = now(), last_poll_ok_at = now(), last_error = NULL, failures = 0,"
	        " playing_now = $1::integer,"
	        " idle_polls = CASE WHEN $1::integer > 0 THEN 0 ELSE least(idle_polls + 1, 20) END,"
	        " next_poll_at = now() + (CASE WHEN $1::integer > 0 THEN $2::double precision"
	        "  ELSE greatest($2::double precision, least($2::double precision * power(2, least(idle_polls + 1, 20)),"
	        "   $3::double precision)) END) * interval '1 second' WHERE server_kind = $4::text",
	        4, params, NULL, error) ||
	    !axis_db_conn_commit(conn, error))
		goto out;
	ok = TRUE;
out:
	if (conn != NULL) {
		axis_db_conn_rollback(conn, NULL);
		axis_db_pool_release(self->pool, conn);
	}
	g_clear_pointer(&writer.map, g_ptr_array_unref);
	g_clear_pointer(&writer.roots, g_ptr_array_unref);
	g_clear_pointer(&writer.libraries, g_ptr_array_unref);
	axis_compat_identity_clear(&identity);
	return ok;
}

/* One kind of server, looked at once. */
typedef struct
{
	AxisCompat            *self;
	const AxisCompatKind  *kind;
	gboolean               force;
	GCancellable          *cancellable;
	AxisCompatPollSummary  summary;
	GError                *error;
	gboolean               active;	/* configured */
	gboolean               ok;	/* looked at, and it went well */
} KindPoll;

static void
poll_kind(KindPoll *job)
{
	AxisCompatConnection connection;
	PollState state;
	g_autoptr(GError) local = NULL;

	memset(&job->summary, 0, sizeof(job->summary));
	if (!axis_compat_connection_load(job->self, job->kind, &connection, &local)) {
		/* Not set up (or switched off): nothing to do, and no failure. */
		job->summary.skipped_poll = TRUE;
		return;
	}
	if (!load_state(job->self, job->kind, &state, &job->error)) {
		axis_compat_connection_clear(&connection);
		return;
	}
	if (!job->force && !state.due) {
		job->summary.skipped_poll = TRUE;
		poll_state_clear(&state);
		axis_compat_connection_clear(&connection);
		return;
	}
	job->ok = do_poll(job->self, &connection, &state, job->cancellable, &job->summary, &local);
	if (!job->ok) {
		if (g_error_matches(local, AXIS_JOB_ERROR, AXIS_JOB_ERROR_CANCELLED)) {
			g_propagate_error(&job->error, g_steal_pointer(&local));
		} else {
			/* The message came from the helper and our fixed text; blank
			 * the token out again in case anything else put it there. */
			g_autofree gchar *clean = axis_http_scrub(local->message, connection.token, 500);

			g_clear_error(&local);
			g_set_error_literal(&local, AXIS_COMPAT_ERROR, AXIS_COMPAT_ERROR_SERVER, clean);
			axis_compat_note_failure(job->self, job->kind->name, local, FALSE);
			g_propagate_error(&job->error, g_steal_pointer(&local));
		}
	}
	poll_state_clear(&state);
	axis_compat_connection_clear(&connection);
}

static gpointer
poll_thread(gpointer data)
{
	poll_kind(data);
	return NULL;
}

gboolean
axis_compat_poll(
	AxisCompat            *self,
	gboolean               force,
	GCancellable          *cancellable,
	AxisCompatPollSummary *summary,
	GError               **error
){
	AxisCompatPollSummary local_summary;
	KindPoll jobs[8];
	GThread *threads[8];
	guint n = 0;
	guint active = 0;
	gboolean any_ok = FALSE;
	gboolean all_skipped = TRUE;
	GError *first_error = NULL;
	guint i;

	g_return_val_if_fail(self != NULL, FALSE);

	if (summary == NULL)
		summary = &local_summary;
	memset(summary, 0, sizeof(*summary));
	memset(jobs, 0, sizeof(jobs));
	memset(threads, 0, sizeof(threads));
	for (i = 0; axis_compat_kind_at(i) != NULL && n < G_N_ELEMENTS(jobs); i++) {
		AxisCompatConnection connection;

		if (!axis_compat_connection_load(self, axis_compat_kind_at(i), &connection, NULL))
			continue;
		axis_compat_connection_clear(&connection);
		jobs[n].self = self;
		jobs[n].kind = axis_compat_kind_at(i);
		jobs[n].force = force;
		jobs[n].cancellable = cancellable;
		jobs[n].active = TRUE;
		n++;
		active++;
	}
	if (active == 0) {
		summary->skipped_poll = TRUE;
		return TRUE;
	}

	/* Each server in a thread of its own when there are several, so that
	 * one that is slow or silent holds up none of the others (the HTTP
	 * helper's deadline bounds each). */
	if (active == 1) {
		poll_kind(&jobs[0]);
	} else {
		for (i = 0; i < n; i++)
			threads[i] = g_thread_new("compat-poll", poll_thread, &jobs[i]);
		for (i = 0; i < n; i++)
			g_thread_join(threads[i]);
	}

	for (i = 0; i < n; i++) {
		summary->playing += jobs[i].summary.playing;
		summary->recorded += jobs[i].summary.recorded;
		summary->updated += jobs[i].summary.updated;
		summary->skipped += jobs[i].summary.skipped;
		summary->matched += jobs[i].summary.matched;
		all_skipped = all_skipped && jobs[i].summary.skipped_poll;
		any_ok = any_ok || jobs[i].ok;
		if (jobs[i].error != NULL && first_error == NULL)
			first_error = g_steal_pointer(&jobs[i].error);
		g_clear_error(&jobs[i].error);
	}
	summary->skipped_poll = all_skipped;
	if (any_ok) {
		gint now = (gint)(g_get_monotonic_time() / G_USEC_PER_SEC) + 1;
		gint last = g_atomic_int_get(&self->retention_run_s);

		if (last == 0 || now - last > RETENTION_EVERY_SECONDS) {
			g_atomic_int_set(&self->retention_run_s, now);
			axis_compat_apply_retention(self, NULL, NULL, NULL);
		}
	}
	if (first_error != NULL) {
		g_propagate_error(error, first_error);
		return FALSE;
	}
	return TRUE;
}
