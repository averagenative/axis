/*
 * axis-meta-private.h - shared by the files of the metadata layer
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef AXIS_META_PRIVATE_H
#define AXIS_META_PRIVATE_H

#include "core/axis-http-origin.h"
#include "db/axis-db-conn.h"
#include "meta/axis-meta.h"

G_BEGIN_DECLS

/* A fetch nobody waits for may wait this long for its provider's budget. */
#define AXIS_META_BACKGROUND_MS 60000

/* How many empty answers, on refreshes spread over a day, make an emptied
 * season a real one (0058_empty_seasons.sql). */
#define AXIS_META_EMPTY_ANSWERS 3

/* Fetch threads per provider. */
#define AXIS_META_FETCH_THREADS 2

/* One transaction-level advisory lock serialises the writes of titles (so
 * that two answers about one title cannot both insert it), and another the
 * artwork files' arrival and removal. */
#define AXIS_META_TITLE_LOCK   G_GINT64_CONSTANT(0x617869734d455401)
#define AXIS_META_ART_LOCK     G_GINT64_CONSTANT(0x617869734d455402)

typedef struct
{
	const AxisMetaProviderInfo *info;
	AxisHttpOrigin              api;
	AxisHttpOrigin              image;
	AxisMetaLimiter            *limiter;
	GThreadPool                *threads;
	const gchar                *key_setting;	/* interned */
	const gchar                *rate_setting;	/* interned */
	const gchar                *pin_setting;	/* interned, nullable */
	/* A managed series follows one of its sources (axis_meta_set_followed()). */
	gint                        followed;	/* atomic */

	/* A provider that logs in (info->login): its token, in memory only.  One
	 * login at a time: the others wait for it, no longer than its deadline. */
	GMutex                      token_lock;
	GCond                       login_done;	/* (token_lock) a login ended */
	gboolean                    logging_in;	/* (token_lock) */
	gchar                      *token;		/* (token_lock) SECRET, wiped */
	gchar                      *token_for;	/* (token_lock) the fingerprint of the key and PIN it was got with */
	gint64                      token_us;	/* (token_lock) monotonic: when it was got */
	gchar                      *refused;	/* (token_lock) the fingerprint of a key and PIN a login refused */
	gchar                      *test_text;	/* (token_lock) what the latest "Test the key" said */
	gboolean                    test_ok;	/* (token_lock) */
	gint64                      test_us;	/* (token_lock) real time */
} AxisMetaProviderEntry;

typedef struct
{
	gchar *name;
	gchar *sql;
} AxisMetaReference;

struct _AxisMeta
{
	gint           refs;		/* atomic */
	AxisDbPool    *pool;
	AxisSettings  *settings;	/* owned reference, nullable */
	AxisDataDir   *data;		/* owned reference, nullable */
	gint           art_fd;		/* owned by @data; -1 without one */
	GPtrArray     *providers;	/* AxisMetaProviderEntry */
	GPtrArray     *references;	/* AxisMetaReference */
	AxisMetaTuning tuning;
	GCancellable  *cancel;		/* cancelled by axis_meta_stop() */
	gint           enabled;		/* atomic */
	gint           stopped;		/* atomic */

	GMutex         lock;
	GCond          done;		/* (lock) a pending fetch ended */
	GHashTable    *inflight;	/* (lock) key -> AxisMetaPending */
	GHashTable    *answered;	/* (lock) key -> when it was last answered (monotonic us, boxed) */
};

/* ── Fetching (axis-meta-fetch.c) ───────────────────────────────── */

typedef enum
{
	AXIS_META_ASK_SEARCH,
	AXIS_META_ASK_FIND,
	AXIS_META_ASK_TITLE,
	AXIS_META_ASK_SEASON,
	AXIS_META_ASK_ART,
	AXIS_META_ASK_ORDER
} AxisMetaAsk;

/* One question to one provider, and its outcome. */
typedef struct
{
	gint                   refs;		/* (meta lock) */
	gchar                 *key;
	AxisMetaAsk            ask;
	AxisMetaProviderEntry *provider;
	gint64                 began_us;	/* monotonic: when the caller began looking (before it read the store) */
	AxisMetaKind           kind;
	gchar                 *text;		/* search: what is sent; title, season, order: the provider's id; find: the scheme */
	gchar                 *query;		/* search: the saved search's key; find: the value; season, order: the source */
	gint                   number;		/* search: the year; season: its number */
	guint                  page;		/* search: the page of the answer (0 is 1) */
	AxisMetaArtSize        size;		/* art; text is the reference */
	gboolean               finished;	/* (meta lock) */
	gboolean               ok;		/* (meta lock) */
	GError                *error;		/* (meta lock) */
} AxisMetaPending;

struct _AxisMetaFetch
{
	AxisMeta              *meta;
	AxisMetaProviderEntry *provider;
	gint64                 deadline_us;	/* monotonic */
	GCancellable          *cancellable;
	/* During a login only (axis_meta_fetch_login()). */
	const gchar           *login_key;	/* SECRET, borrowed */
	const gchar           *login_pin;	/* SECRET, borrowed, nullable */
	gboolean               login_refused;	/* the login was answered 401 or 403 */
};

/* Wipes and frees a provider's token state (axis-meta.c's entry_free()). */
void
axis_meta_fetch_clear_provider(AxisMetaProviderEntry *provider);

/* The login fields of @status: refused, a token held, the latest test. */
void
axis_meta_fetch_login_status(
	AxisMeta               *self,
	AxisMetaProviderEntry  *provider,
	AxisMetaProviderStatus *status
);

AxisMetaPending *
axis_meta_pending_new(
	AxisMetaAsk            ask,
	AxisMetaProviderEntry *provider
);

/* Runs @pending on a fetch thread of its provider unless the same question
 * is being fetched already, and waits for it at most @wait_ms (0: does not
 * wait at all).  Takes @pending.  Returns: %TRUE if it ended in time with
 * success; otherwise *@error says why (also "not answered in time"). */
gboolean
axis_meta_ask(
	AxisMeta         *self,
	AxisMetaPending  *pending,
	guint             wait_ms,
	GError          **error
);

/* Runs @pending here and now (for the refresh job), storing what it gets. */
gboolean
axis_meta_ask_now(
	AxisMeta         *self,
	AxisMetaPending  *pending,
	GCancellable     *cancellable,
	GError          **error
);

void
axis_meta_pending_unref(
	AxisMeta        *self,
	AxisMetaPending *pending
);

/* Creates the fetch threads of a provider (axis-meta.c calls it). */
void
axis_meta_fetch_init_provider(
	AxisMeta              *self,
	AxisMetaProviderEntry *provider
);

/* The provider's key, or %NULL; free with axis_meta_secret_free(). */
gchar *
axis_meta_provider_key(
	AxisMeta                    *self,
	const AxisMetaProviderEntry *provider
);

void
axis_meta_secret_free(gchar *secret);

/* A GET on the image origin (no key), within the budget. */
gboolean
axis_meta_fetch_image(
	AxisMetaFetch  *fetch,
	const gchar    *path,
	GBytes        **body,
	gchar         **content_type,
	GError        **error
);

AxisMetaProviderEntry *
axis_meta_entry_find(
	AxisMeta    *self,
	const gchar *name
);

/* ── The store (axis-meta-store.c) ──────────────────────────────── */

/* Stores @title (its provider's answer) in one transaction: found by any of
 * its identifiers and updated in place, or added; two records that turn
 * out to be one title are merged.  A field the answer lacks keeps what was
 * stored. */
gboolean
axis_meta_store_title(
	AxisDbConn           *conn,
	const AxisMetaTitle  *title,
	gint64               *id,
	GError              **error
);

/* As axis_meta_store_title(), for an answer of a @secondary provider: one
 * about a title another provider's record made adds its identifiers, other
 * names and own seasons only.  Every store follows the cross-id rule: a claim
 * about another scheme's identifier never merges two titles that each hold
 * their own provider's record (meta_id_claim keeps it). */
gboolean
axis_meta_store_title_from(
	AxisDbConn           *conn,
	const AxisMetaTitle  *title,
	gboolean              secondary,
	gint64               *id,
	GError              **error
);

/* The stored title @id: with its ids, and when @full its other names and
 * seasons; *@title %NULL when there is none. */
gboolean
axis_meta_store_load_title(
	AxisDbConn     *conn,
	gint64          id,
	gboolean        full,
	AxisMetaTitle **title,
	GError        **error
);

/* The brief records of @ids, in that order (missing ones left out). */
gboolean
axis_meta_store_load_titles(
	AxisDbConn  *conn,
	GArray      *ids,
	GPtrArray  **titles,
	GError     **error
);

/* The stored title of @kind with identifier @scheme:@value, or 0. */
gboolean
axis_meta_store_find_id(
	AxisDbConn   *conn,
	AxisMetaKind  kind,
	const gchar  *scheme,
	const gchar  *value,
	gint64       *id,
	GError      **error
);

/* A saved lookup: *@found, its result ids and when it was fetched. */
gboolean
axis_meta_store_get_lookup(
	AxisDbConn   *conn,
	const gchar  *provider,
	AxisMetaKind  kind,
	const gchar  *lookup,
	const gchar  *query,
	gint          year,
	gboolean     *found,
	GArray      **ids,
	gint64       *fetched_us,
	GError      **error
);

gboolean
axis_meta_store_put_lookup(
	AxisDbConn   *conn,
	const gchar  *provider,
	AxisMetaKind  kind,
	const gchar  *lookup,
	const gchar  *query,
	gint          year,
	GArray       *ids,
	GError      **error
);

/* Stores a season of @source with its episodes (replacing the episodes
 * stored for that source). */
gboolean
axis_meta_store_season(
	AxisDbConn            *conn,
	gint64                 title_id,
	const gchar           *source,
	const AxisMetaSeason  *season,
	GError               **error
);

/* Stores every season of @source with its episodes in one transaction, the
 * seasons of @source it lacks removed; no other source's season changes. */
gboolean
axis_meta_store_order(
	AxisDbConn       *conn,
	gint64            title_id,
	const gchar      *source,
	const GPtrArray  *seasons,
	GError          **error
);

/* The stored season of @source with its episodes; *@season %NULL when there
 * is none.  *@is_current: it is the source's latest season. */
gboolean
axis_meta_store_load_season(
	AxisDbConn      *conn,
	gint64           title_id,
	const gchar     *source,
	gint             number,
	AxisMetaSeason **season,
	gboolean        *is_current,
	GError         **error
);

/* The title was shown on a page now (written at most hourly). */
gboolean
axis_meta_store_mark_viewed(
	AxisDbConn  *conn,
	gint64       id,
	GError     **error
);

/* Today's date by the database's clock, "YYYY-MM-DD". */
gchar *
axis_meta_store_today(
	AxisDbConn  *conn,
	GError     **error
);

/* ── Artwork (axis-meta-art.c) ──────────────────────────────────── */

/* Runs an artwork question (a pending fetch of kind ART). */
gboolean
axis_meta_art_fetch_now(
	AxisMeta         *self,
	AxisMetaFetch    *fetch,
	const gchar      *ref,
	AxisMetaArtSize   size,
	gchar           **hash,
	GError          **error
);

/* ── Helpers ────────────────────────────────────────────────────── */

/* The text form of a PostgreSQL text[] holding @items. */
gchar *
axis_meta_sql_text_array(GPtrArray *items);

/* The text form of a bigint[]. */
gchar *
axis_meta_sql_int_array(GArray *ids);

/* Store and database errors as the layer's (the message kept). */
void
axis_meta_set_failed(
	GError      **error,
	const GError *cause
);

G_END_DECLS

#endif /* AXIS_META_PRIVATE_H */
