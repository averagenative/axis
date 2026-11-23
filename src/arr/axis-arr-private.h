/*
 * axis-arr-private.h - shared by the files of the bring-over layer
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 *   axis-arr.c          the layer, the connections and their test
 *   axis-arr-client.c   the requests: GET only, the key in its header
 *   axis-arr-parse.c    reading the answers (pure; fuzzed)
 *   axis-arr-read.c     the job arr.read, the plan, the clean-up
 *   axis-arr-preview.c  the preview and the mappings
 *   axis-arr-apply.c    the run (the job arr.apply), what was brought, and
 *                       starting the searches
 */

#ifndef AXIS_ARR_PRIVATE_H
#define AXIS_ARR_PRIVATE_H

#include <gio/gio.h>

#include "arr/axis-arr.h"
#include "core/axis-http.h"
#include "db/axis-db-conn.h"

G_BEGIN_DECLS

#define AXIS_ARR_US(column) "(extract(epoch FROM " column ") * 1000000)::bigint"

struct _AxisArr
{
	gint           ref_count;
	AxisDbPool    *pool;
	AxisMeta      *meta;
	GCancellable  *cancel;	/* cancelled when the layer stops */
	GMutex         lock;	/* @tuning */
	AxisArrTuning  tuning;
	AxisMovies    *movies;	/* (nullable) owned: what a run adds movies through */
	AxisTv        *tv;	/* (nullable) owned: what a run adds series through */
};

/* ── What a run would do (axis-arr-preview.c) ───────────────────── */

/* One episode file of a series a run would bring (or link later): the app's
 * own number of it, where it stands, and its row of the index when it is
 * found at its mapped path with the size the app recorded; @other_size_id:
 * its row when it is there with another size (%AXIS_ARR_FILE_OTHER_SIZE),
 * which a run never links and only reports on. */
typedef struct
{
	gint64            arr_id;
	AxisArrFileState  state;
	gint64            file_id;
	gint64            other_size_id;
} AxisArrFileDecision;

/**
 * AxisArrDecision:
 * @seq: the item of the plan
 * @relink: not a title to bring: one this connection brought before, of
 *   which Axis holds a file now that it did not link then
 * @profile_id, @root_id: Axis's quality profile and library folder for it
 * @file: (a movie) where its file stands; @file_id: its row of the index when
 *   it is found (%AXIS_ARR_FILE_FOUND); @other: (nullable) the movie it is
 *   linked to
 * @files: (a series) (nullable) (element-type AxisArrFileDecision): its
 *   episode files, in the plan's order; for a @relink, only those to link
 */
typedef struct
{
	guint             seq;
	gint64            arr_id;
	gint64            title_id;
	gboolean          relink;
	gint64            profile_id;
	gint64            root_id;
	gboolean          has_file;
	AxisArrFileState  file;
	gint64            file_id;
	gchar            *other;
	GArray           *files;
} AxisArrDecision;

void
axis_arr_decision_free(gpointer data);

/* axis_arr_preview(), and with @decisions (nullable) every movie a run would
 * bring and every file it would link later, in the plan's order
 * (element-type AxisArrDecision; %NULL when there is no plan).  A Radarr's
 * decisions are of movies and a Sonarr's of series (each with its episode
 * files): the run hands each to the module of the connection's app, so
 * nothing can bring a series as if it were a movie. */
gboolean
axis_arr_preview_decide(
	AxisArr         *self,
	gint64           source_id,
	AxisArrSkip      skip_filter,
	guint            skip_page,
	guint            file_page,
	AxisArrPreview **preview,
	GPtrArray      **decisions,
	GError         **error
);

/* A seam for tests: called by the run (axis_arr_run_apply()) before it takes
 * the title at @index of what it brings; %FALSE with @error set ends the run
 * there as a failure would (what was brought stays, each title whole).  %NULL
 * in service. */
extern gboolean (*axis_arr_apply_hook)(guint index, gpointer data, GError **error);
extern gpointer axis_arr_apply_hook_data;

/* The tuning in effect, defaults filled in. */
void
axis_arr_get_tuning(
	AxisArr       *self,
	AxisArrTuning *tuning
);

/* ── Results ────────────────────────────────────────────────────── */

gchar *
axis_arr_text_at(
	AxisDbResult *result,
	guint         row,
	guint         column
);

gint64
axis_arr_int_at(
	AxisDbResult *result,
	guint         row,
	guint         column,
	gint64        fallback
);

gboolean
axis_arr_bool_at(
	AxisDbResult *result,
	guint         row,
	guint         column
);

/* Frees @secret after overwriting it. */
void
axis_arr_secret_free(gchar *secret);

/* ── The store (axis-arr.c) ─────────────────────────────────────── */

/* The connection @id, or %AXIS_ARR_ERROR_NOT_FOUND. */
gboolean
axis_arr_store_source(
	AxisDbConn     *conn,
	gint64          id,
	AxisArrSource **source,
	GError        **error
);

/* The one place the key is read: for a request.  The connection and its key
 * come from one statement, so the key is the one stored for the address
 * returned with it.  *@secret is %NULL when none is stored; free it with
 * axis_arr_secret_free(). */
gboolean
axis_arr_store_source_secret(
	AxisDbConn     *conn,
	gint64          id,
	AxisArrSource **source,
	gchar         **secret,
	GError        **error
);

/* What a test or a reading found of the connection at @origin and
 * @path_prefix, the address it was made at: when the address was changed
 * since, nothing is written. */
gboolean
axis_arr_store_test(
	AxisArr      *self,
	gint64        id,
	const gchar  *origin,
	const gchar  *path_prefix,
	AxisArrTest   result,
	const gchar  *version,
	GError      **error
);

/* ── The client (axis-arr-client.c) ─────────────────────────────── */

typedef struct _AxisArrClient AxisArrClient;

/**
 * axis_arr_client_new:
 * @source: the connection
 * @secret: its key; copied into the sessions, which wipe it when freed
 * @cancellable: (nullable): cancels its requests (a job's)
 *
 * Three sessions of purpose %AXIS_HTTP_PURPOSE_READ to @source's origin: one
 * for the list of every movie or series (its size limit and deadline), one
 * for a list Sonarr gives of one series, one for everything else.  None can
 * send anything but GET.
 */
AxisArrClient *
axis_arr_client_new(
	AxisArr              *self,
	const AxisArrSource  *source,
	const gchar          *secret,
	GCancellable         *cancellable,
	GError              **error
);

void
axis_arr_client_free(AxisArrClient *client);

/* Which limits a request is made under. */
typedef enum
{
	AXIS_ARR_GET_SMALL,	/* any ordinary answer: 16 MB, 15 seconds */
	AXIS_ARR_GET_LIBRARY,	/* the list of every movie or series: the large limit, the long deadline */
	AXIS_ARR_GET_SERIES	/* one series' episodes or files (Sonarr) */
} AxisArrGet;

/**
 * axis_arr_client_get:
 * @path: a path of axis-arr-assumed.h (the connection's prefix is put
 *   before it)
 * @kind: the limits it is made under
 * @body: (out) (transfer full): the answer's body
 *
 * One GET.  An error is one of %AXIS_ARR_ERROR in fixed words naming the
 * app: nothing the app or the HTTP library said is in it.
 */
gboolean
axis_arr_client_get(
	AxisArrClient  *client,
	const gchar    *path,
	AxisArrGet      kind,
	GBytes        **body,
	GError        **error
);

/* The path of page @page of @app's download queue (A9). */
gchar *
axis_arr_queue_path(
	AxisArrApp app,
	guint      page,
	guint      page_size
);

/* The session of ordinary requests; for the test that a request which is
 * not a GET fails without sending. */
AxisHttpSession *
axis_arr_client_peek_session(AxisArrClient *client);

/* GET system/status, and whether the answer names the connection's app:
 * %AXIS_ARR_ERROR_NOT_APP when it does not. */
gboolean
axis_arr_client_status(
	AxisArrClient  *client,
	gchar         **version,
	GError        **error
);

/* What an error of the client is, as a test's result. */
AxisArrTest
axis_arr_test_of_error(const GError *error);

G_END_DECLS

#endif /* AXIS_ARR_PRIVATE_H */
