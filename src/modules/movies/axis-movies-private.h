/*
 * axis-movies-private.h - shared by the files of the movies module
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 *   axis-movies.c          lifetime, settings, the module, adding, editing,
 *                          removing, queueing a search, the blocklist
 *   axis-movies-state.c    states and their derivation (pure)
 *   axis-movies-store.c    the facts query and the module's other SQL
 *   axis-movies-library.c  the movies on disk Axis does not manage: their
 *                          facts and their page, read at query time
 *   axis-movies-list.c     a page of the list (both kinds), at any size
 *   axis-movies-search.c   the films' side of the acquisition layer (what
 *                          a film wants, its claim, its history), the
 *                          search and the manual grab
 *   axis-movies-follow.c   the follow-up of grabs, the scheduler
 *   axis-movies-links.c    a movie's files linked to it by hand
 *   axis-movies-web*.c     the pages, the title actions, the dashboard
 */

#ifndef AXIS_MOVIES_PRIVATE_H
#define AXIS_MOVIES_PRIVATE_H

#include "modules/movies/axis-movies.h"

#include "db/axis-db-conn.h"
#include "jobs/axis-job-context.h"
#include "web/axis-web-attention.h"

G_BEGIN_DECLS

/* Tasks waiting for a thread beyond this are answered 503. */
#define AXIS_MOVIES_MAX_PENDING 16

struct _AxisMovies
{
	gint               refs;	/* atomic */
	AxisDbPool        *pool;
	AxisSettings      *settings;	/* (nullable) owned */
	AxisMeta          *meta;	/* owned */
	AxisIndexers      *indexers;	/* owned */
	AxisDownloads     *downloads;	/* owned */
	AxisStorage       *storage;	/* owned */
	AxisQualityStore  *quality;	/* owned */
	AxisImport        *import;	/* (nullable) borrowed: outlives the module */
	AxisAcquire       *acquire;	/* owned: searching, grabbing, following */

	/* The pages' work off the main loop (axis-movies-web.c). */
	guint              pending;	/* (main loop) */
	gboolean           stopping;	/* (main loop) */

	/* What the page "Needs attention" was last told of the movies (the
	 * source of axis-movies-web.c, which alone touches it). */
	AxisAttentionTitles attention;
};

/* ── Settings (axis-movies.c) ───────────────────────────────────── */

const AxisSettingInfo *
axis_movies_setting_table(void);

gint64
axis_movies_setting_duration(
	AxisMovies  *self,
	const gchar *key,
	gint64       fallback
);

gint64
axis_movies_setting_integer(
	AxisMovies  *self,
	const gchar *key,
	gint64       fallback
);

/* A choice or string setting, or @fallback. */
gchar *
axis_movies_setting_string(
	AxisMovies  *self,
	const gchar *key,
	const gchar *fallback
);

/* movies.languages: the ISO 639-1 codes in it (others are left out), as a
 * %NULL-ended vector; empty for none. */
gchar **
axis_movies_setting_languages(AxisMovies *self);

/* ── Small helpers (the acquisition layer's) ────────────────────── */

/* "%" G_GINT64_FORMAT */
#define axis_movies_int_text axis_acquire_int_text

/* A text cut to @max characters on a character boundary, valid UTF-8, with
 * control characters made spaces; %NULL for %NULL or nothing left. */
#define axis_movies_text_cut axis_acquire_text_cut

/* The label a film has in history and downloads: "Title (Year)". */
gchar *
axis_movies_title_label(
	const gchar *title,
	gint         year
);

/* "2026-11-14" for @now_us, UTC. */
void
axis_movies_today(
	gint64 now_us,
	gchar  today[11]
);

/* ── The store (axis-movies-store.c) ────────────────────────────── */

typedef enum
{
	AXIS_MOVIES_FACTS_ALL,		/* no parameter */
	AXIS_MOVIES_FACTS_ONE,		/* $1: a film's id */
	AXIS_MOVIES_FACTS_TITLES,	/* $1: bigint[] of meta_title ids */
	AXIS_MOVIES_FACTS_MONITORED,	/* no parameter */
	AXIS_MOVIES_FACTS_IDS		/* $1: bigint[] of film ids (a page of a list) */
} AxisMoviesFactsFilter;

/**
 * axis_movies_store_facts:
 * @param: (nullable): the filter's parameter
 * @films: (out) (transfer full) (element-type AxisMovieFacts): by title
 *
 * THE facts query: everything a film's state is derived from, read in one
 * statement on @conn (so inside a transaction it sees what the transaction
 * sees).
 */
gboolean
axis_movies_store_facts(
	AxisDbConn             *conn,
	AxisMoviesFactsFilter   filter,
	const gchar            *param,
	GPtrArray             **films,
	GError                **error
);

/* One film's facts on @conn; %NULL (and %TRUE) when there is none. */
gboolean
axis_movies_store_facts_one(
	AxisDbConn      *conn,
	gint64           movie_id,
	AxisMovieFacts **facts,
	GError         **error
);

/* Appends to the film's history (on @conn, in the caller's transaction or
 * not).  @movie_id 0 with @title_label for a film already gone. */
gboolean
axis_movies_store_event(
	AxisDbConn   *conn,
	gint64        movie_id,
	gint64        title_id,
	const gchar  *title_label,
	const gchar  *kind,
	const gchar  *text,
	const gchar  *user,
	gint64        search_id,
	gint64        grab_id,
	GError      **error
);

/* printf-style; the text is cut to the column's limit. */
gboolean
axis_movies_store_eventf(
	AxisDbConn         *conn,
	const AxisMovieFacts *facts,
	const gchar        *kind,
	const gchar        *user,
	gint64              search_id,
	gint64              grab_id,
	GError            **error,
	const gchar        *format,
	...
) G_GNUC_PRINTF(8, 9);

/* Locks the film's row (SELECT ... FOR UPDATE) in the open transaction;
 * *@found is %FALSE when there is no such film. */
gboolean
axis_movies_store_lock(
	AxisDbConn  *conn,
	gint64       movie_id,
	gboolean    *found,
	GError     **error
);

/* Adding a film, in the caller's open transaction: what axis_movies_add()
 * does, and what bringing a movie over goes through (axis-movies-brought.c). */
gboolean
axis_movies_add_in_transaction(
	AxisMovies          *self,
	AxisDbConn          *conn,
	const AxisMovieSpec *spec,
	gint64              *movie_id,
	GError             **error
);

/* Ends the hold of a movie brought over (movie.search_hold), when it has
 * one, because a person did @what ("Search now", "a manual search"); its
 * history says so.  In the caller's transaction (axis-movies-brought.c). */
gboolean
axis_movies_store_end_hold(
	AxisDbConn            *conn,
	const AxisMovieFacts  *facts,
	const gchar           *user,
	const gchar           *what,
	GError               **error
);

/* Queues the film's search job on @conn (de-duplicated by the film). */
gboolean
axis_movies_store_enqueue_search(
	AxisDbConn   *conn,
	gint64        movie_id,
	const gchar  *cause,
	const gchar  *user,
	gint64       *job_id,
	gboolean     *created,
	GError      **error
);

/* ── The films' side of the acquisition layer (axis-movies-search.c) ── */

/* What the acquisition layer is given for the films. */
const AxisAcquireOwnerInfo *
axis_movies_acquire_info(void);

/* ── Imports (axis-movies-import.c) ─────────────────────────────── */

/* The follow-up's part: an import asked for every completed grab (and the
 * waiting ones looked at again). */
gboolean
axis_movies_request_imports(
	AxisMovies             *self,
	AxisMovieFollowReport  *report,
	GError                **error
);

/* The best of a list of "quality:resolution" pairs (a facts column): a
 * ladder quality, or an unknown source at a resolution. */
void
axis_movies_best_on_disk(
	const gchar           *list,
	AxisQuality           *quality,
	AxisReleaseResolution *resolution
);

/* The volume states a disk is usable in, as a text[] ("{online,...}"). */
gchar *
axis_movies_usable_states(void);

/* A file of a film's page from a row of: lf.id, root name, volume name,
 * rel_path, size, state, quality, quality_text, pinned, import_id, import
 * mode, linked (us), resolution, identity_id, linked_how, best, away. */
AxisMovieFile *
axis_movies_file_from_row(
	AxisDbResult *rows,
	guint         row
);

/* The history a title kept of films removed before (movie_id NULL), the
 * latest first. */
gboolean
axis_movies_store_title_events(
	AxisDbConn   *conn,
	gint64        title_id,
	guint         limit,
	GPtrArray   **events,
	GError      **error
);

/* ── Movies on disk that Axis does not manage (axis-movies-library.c) ── */

/*
 * The units identification linked (in a root for movies) that hold a file
 * present in the index and linked to no film, the "on disk" of a movie on
 * disk.  An offline disk freezes its rows as present (the scanner marks a
 * file absent only after walking a disk verified online), so a movie whose
 * disk is away stays listed; a file a scan found gone is left out.  li is
 * the unit, llr its root, llf the file, llif its row in the unit: the
 * tables, then the conditions (the caller may join more between them).
 */
#define AXIS_MOVIES_ON_DISK_TABLES \
	" library_identity li JOIN library_root llr ON llr.id = li.root_id AND llr.media_type = 'movies'" \
	" JOIN library_identity_file llif ON llif.identity_id = li.id" \
	" JOIN library_file llf ON llf.id = llif.file_id AND llf.state = 'present'"

#define AXIS_MOVIES_ON_DISK_WHERE \
	" WHERE li.state = 'linked' AND li.title_id IS NOT NULL" \
	" AND NOT EXISTS (SELECT 1 FROM movie_file lmf WHERE lmf.file_id = llf.id)" \
	" AND NOT EXISTS (SELECT 1 FROM movie lm WHERE lm.title_id = li.title_id)"

/*
 * The movies on disk that Axis does not manage, one row per title: lib
 * (title_id, found_at: when identification first linked one of its units,
 * size: of its files there) and t, its meta_title (a film with no film
 * row).  The caller adds a WHERE clause.
 */
#define AXIS_MOVIES_LIBRARY_FROM \
	" FROM (SELECT li.title_id, min(coalesce(li.decided_at, li.created_at)) AS found_at," \
	"   coalesce(sum(llf.size), 0) AS size FROM" AXIS_MOVIES_ON_DISK_TABLES AXIS_MOVIES_ON_DISK_WHERE \
	"   GROUP BY li.title_id) lib JOIN meta_title t ON t.id = lib.title_id AND t.kind = 'film'"

/**
 * axis_movies_store_library:
 * @titles: a bigint[] of meta_title ids
 * @films: (out) (transfer full) (element-type AxisMovieFacts): the movies on
 *   disk among them that Axis does not manage, by title
 *
 * One statement on @conn, whatever the number of their files.
 */
gboolean
axis_movies_store_library(
	AxisDbConn   *conn,
	const gchar  *titles,
	GPtrArray   **films,
	GError      **error
);

/* Whether @title_id is a movie on disk that Axis does not manage, as @conn
 * sees it (inside a transaction that removed its film: whether it stays
 * listed). */
gboolean
axis_movies_store_on_disk(
	AxisDbConn   *conn,
	gint64        title_id,
	gboolean     *on_disk,
	GError      **error
);

/* ── Identification (axis-movies-identify.c) ────────────────────── */

/* In the caller's transaction: the links of the film @movie_id to every file
 * identified as @title_id (an import's links stand); *@linked: how many. */
gboolean
axis_movies_link_identified(
	AxisDbConn   *conn,
	gint64        movie_id,
	gint64        title_id,
	guint        *linked,
	GError      **error
);

/* ── The pages (axis-movies-web.c) ──────────────────────────────── */

void
axis_movies_web_register(
	AxisMovies *self,
	AxisWeb    *web
);

G_END_DECLS

#endif /* AXIS_MOVIES_PRIVATE_H */
