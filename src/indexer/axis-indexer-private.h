/*
 * axis-indexer-private.h - shared by the files of the indexer layer
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 *   axis-indexer.c         lifetime, kinds, settings
 *   axis-indexer-conn.c    connections to one source: its budget, its calls,
 *                          the mapping of errors
 *   axis-indexer-store.c   the database: sources, keys, indexers, searches,
 *                          releases and their secrets
 *   axis-indexer-search.c  sources' saving and testing, searching, fetching,
 *                          expiry
 *   axis-indexer-fanout.c  a search of every indexer and source at once, and
 *                          the merging of their answers
 *   axis-indexer-newznab.c the Newznab and Torznab kinds (native indexers)
 */

#ifndef AXIS_INDEXER_PRIVATE_H
#define AXIS_INDEXER_PRIVATE_H

#include "core/axis-http.h"
#include "db/axis-db-conn.h"
#include "indexer/axis-indexer.h"
#include "meta/axis-meta-limit.h"

G_BEGIN_DECLS

/*
 * What the layer keeps in memory about one source: its budget (a limiter,
 * shared by every call to it) and how many calls are running.
 */
typedef struct
{
	gint              refs;		/* atomic */
	gint64            id;
	AxisMetaLimiter  *limiter;
	GMutex            lock;
	GCond             cond;
	guint             active;	/* (lock) calls running */
} AxisIndexerSlot;

struct _AxisIndexers
{
	gint               refs;	/* atomic */
	AxisDbPool        *pool;
	AxisSettings      *settings;	/* owned reference, nullable */
	GPtrArray         *kinds;	/* const AxisIndexerKindInfo * */
	AxisIndexerTuning  tuning;
	GCancellable      *cancel;
	gint               stopped;	/* atomic */
	GMutex             lock;
	GHashTable        *slots;	/* (lock) gint64 * -> AxisIndexerSlot * */
	GHashTable        *inflight;	/* (lock) gchar * "source:key" of a search being made */
	GCond              inflight_done;	/* (lock) */
};

/* A source's configuration as a connection needs it. */
typedef struct
{
	AxisIndexerSourceInfo *info;
	gchar                 *secret;	/* wiped */
	gchar                 *definition;	/* the file of the definition it runs, or NULL */
} AxisIndexerTarget;

void
axis_indexer_target_clear(AxisIndexerTarget *target);

/* What a native kind learned during one call, for the layer to store after
 * it (axis_indexer_store_learned()). */
typedef struct
{
	gboolean              caps_learned;
	AxisIndexerCaps       caps;
	gboolean              allowance_learned;
	AxisIndexerAllowance  allowance;
	gchar                *other_origin;
	gint64                held_until_us;
	gint64                grabs_held_until_us;
	gchar                *hold_message;
	guint                 n_calls;	/* requests made to the indexer's API */
	guint                 n_grabs;	/* files fetched from it */
} AxisIndexerLearned;

void
axis_indexer_learned_clear(AxisIndexerLearned *learned);

struct _AxisIndexerConn
{
	AxisIndexers               *owner;
	const AxisIndexerKindInfo  *kind;
	AxisIndexerSlot            *slot;
	AxisHttpOrigin              origin;	/* a copy */
	gchar                      *base_path;	/* borrowed from the target */
	gchar                      *secret;	/* borrowed from the target */
	const gchar                *label;	/* borrowed from the target */
	gboolean                    accept_untrusted;
	gboolean                    follow_redirects;	/* a release's download, to the indexer's site */
	GPtrArray                  *indexers;	/* borrowed: the target's, or a list read since */
	GCancellable               *cancel;	/* this call's */
	gint64                      deadline_us;	/* monotonic: how long a request may wait for its budget */
	/* A native indexer's: */
	gboolean                    has_native;
	AxisIndexerNative           native;	/* points into the target and @learned */
	AxisHttpOrigin              files_origin;	/* a copy; .base NULL for none */
	AxisIndexerLearned          learned;
	const gchar                *definition;	/* borrowed from the target */
	struct _AxisDefTrace       *trace;	/* nullable, borrowed: a check's */
};

/* A setting's value, or @fallback without a settings store. */
gint64
axis_indexer_setting_duration(
	AxisIndexers *self,
	const gchar  *key,
	gint64        fallback
);

gint64
axis_indexer_setting_integer(
	AxisIndexers *self,
	const gchar  *key,
	gint64        fallback
);

gdouble
axis_indexer_setting_decimal(
	AxisIndexers *self,
	const gchar  *key,
	gdouble       fallback
);

/* The slot of source @id, made when there is none; unref it with
 * axis_indexer_slot_unref(). */
AxisIndexerSlot *
axis_indexer_slot_get(
	AxisIndexers *self,
	gint64        id
);

void
axis_indexer_slot_unref(AxisIndexerSlot *slot);

/* Forgets the slot of source @id (its address or key changed: what the
 * limiter learned was about another place). */
void
axis_indexer_forget_source(
	AxisIndexers *self,
	gint64        id
);

/* What a failed contact with a source means for it: its state and a
 * sentence for its page (the error's own words, already free of secrets). */
gchar *
axis_indexer_contact_problem(
	const GError           *failure,
	AxisIndexerSourceState *state
);

/* Whether an error says something about the source (and not about this
 * call: no such source, cancelled, busy, a request that was not valid). */
gboolean
axis_indexer_is_contact_failure(const GError *error);

/* Stores that the source answered (@failure %NULL) or what went wrong. */
void
axis_indexer_record_contact(
	AxisIndexers *self,
	gint64        source_id,
	const GError *failure,
	const gchar  *version
);

/* One indexer's or source's search, as axis_indexers_search() makes it for
 * a source it names (axis-indexer-search.c). */
gboolean
axis_indexer_search_source(
	AxisIndexers              *self,
	gint64                     source_id,
	const AxisIndexerRequest  *request,
	GCancellable              *cancellable,
	AxisIndexerSearchResult  **result,
	GError                   **error
);

/* The search of every enabled one (axis-indexer-fanout.c). */
gboolean
axis_indexer_search_all(
	AxisIndexers              *self,
	const AxisIndexerRequest  *request,
	GCancellable              *cancellable,
	AxisIndexerSearchResult  **result,
	GError                   **error
);

/* ── axis-indexer-conn.c ─────────────────────────────────────────── */

typedef gboolean (*AxisIndexerCallFunc)(
	AxisIndexers               *self,
	const AxisIndexerTarget    *target,
	const AxisIndexerKindInfo  *kind,
	AxisIndexerConn            *conn,
	gpointer                    data,
	GError                    **error
);

/*
 * Loads source @id's configuration and key, takes a place among the calls
 * running at it (waiting for one, at most a while) and runs @func with a
 * connection to it.  Fails with %AXIS_INDEXER_ERROR_NOT_FOUND for no such
 * source, %AXIS_INDEXER_ERROR_INVALID when its kind is not one this daemon
 * has, and %AXIS_INDEXER_ERROR_BUSY when no place came free in time.  With
 * @only_enabled, a source that is switched off is %AXIS_INDEXER_ERROR_NO_SOURCE.
 */
gboolean
axis_indexer_with_source(
	AxisIndexers         *self,
	gint64                id,
	gboolean              only_enabled,
	GCancellable         *cancellable,
	AxisIndexerCallFunc   func,
	gpointer              data,
	GError              **error
);

/* ── axis-indexer-store.c ────────────────────────────────────────── */

/* Every source (or the one of @id, when not 0), by name, with its indexers. */
gboolean
axis_indexer_store_sources(
	AxisDbConn  *conn,
	gint64       id,
	GPtrArray  **sources,
	GError     **error
);

/* The file of installed definition @id; *@body %NULL when there is none. */
/* The file an indexer runs for the definition stored as @def_id with @body
 * and, for a built-in one, the override stored with it (nullable): the
 * built-in copy this daemon ships, or the override when it is newer and
 * usable (indexer/axis-indexer-shipped.h); else @body. */
gchar *
axis_indexer_definition_in_use(
	const gchar *def_id,
	const gchar *body,
	const gchar *override_body
);

/* After the file of the definition @id_text ("123") changed, inside the
 * caller's transaction: its indexers' kept searches are forgotten and a hold
 * for what its answers were is lifted (axis-indexer-definitions.c). */
gboolean
axis_indexer_definition_changed(
	AxisDbConn   *conn,
	const gchar  *id_text,
	GError      **error
);

/* Lifts the hold of the indexer @id, or (@id 0) of every indexer that runs
 * the definition @definition_id, when it runs a definition. */
gboolean
axis_indexer_store_lift_hold(
	AxisDbConn  *conn,
	gint64       id,
	gint64       definition_id,
	GError     **error
);

gboolean
axis_indexer_store_definition_body(
	AxisDbConn  *conn,
	gint64       id,
	gchar      **body,
	GError     **error
);

/* The key of source @id; %NULL when none is set.  The only reader of the
 * keys table. */
gboolean
axis_indexer_store_secret(
	AxisDbConn  *conn,
	gint64       id,
	gchar      **secret,
	GError     **error
);

gboolean
axis_indexer_store_source_state(
	AxisDbConn              *conn,
	gint64                   id,
	AxisIndexerSourceState   state,
	const gchar             *message,
	const gchar             *version,
	gboolean                 contact,
	GError                 **error
);

/* Replaces the list of indexers of source @id (in one transaction). */
gboolean
axis_indexer_store_replace_indexers(
	AxisDbConn  *conn,
	gint64       id,
	GPtrArray   *indexers,
	GError     **error
);

/* What a native indexer's call learned: its capabilities, allowances
 * (reported, or counted on by @learned's calls and grabs), an origin seen,
 * a hold. */
gboolean
axis_indexer_store_learned(
	AxisDbConn               *conn,
	gint64                    id,
	const AxisIndexerLearned *learned,
	GError                  **error
);

/* Accepts the origin of files seen for native indexer @id; *@accepted is
 * %FALSE when there was none. */
gboolean
axis_indexer_store_accept_files_origin(
	AxisDbConn  *conn,
	gint64       id,
	gboolean    *accepted,
	GError     **error
);

/* The other stored, unexpired releases of the same group as @release_id
 * (element-type gint64), of enabled sources, by priority, one for each
 * source and indexer that is not @release_id's own, at most @max. */
gboolean
axis_indexer_store_alternatives(
	AxisDbConn  *conn,
	gint64       release_id,
	guint        max,
	GArray     **ids,
	GError     **error
);

/* The stored, unexpired search of @key at the source, with its releases
 * (never their secrets); *@result %NULL when there is none. */
gboolean
axis_indexer_store_get_search(
	AxisDbConn               *conn,
	gint64                    source_id,
	const gchar              *key,
	AxisIndexerSearchResult **result,
	GError                  **error
);

/**
 * axis_indexer_store_put_search:
 * @outcome: what the kind returned (its items' secrets are stored in their
 *   own table)
 * @failed: (element-type utf8): names of failing indexers
 * @life_s: how long it is kept
 *
 * Replaces any search of the same key (expired, or lost a race) with this
 * one, in one transaction, and returns it with the releases' identifiers.
 */
gboolean
axis_indexer_store_put_search(
	AxisDbConn                *conn,
	gint64                     source_id,
	const gchar               *key,
	const AxisIndexerOutcome  *outcome,
	gint                       n_indexers,
	GPtrArray                 *failed,
	gint64                     life_s,
	AxisIndexerSearchResult  **result,
	GError                   **error
);

/* One stored, unexpired release without its secrets; *@release %NULL when
 * there is none. */
gboolean
axis_indexer_store_get_release(
	AxisDbConn           *conn,
	gint64                release_id,
	AxisIndexerRelease  **release,
	GError              **error
);

/* What a fetch needs of a release: its source, and its secrets.  The only
 * reader of the releases' secrets. */
typedef struct
{
	gint64                source_id;
	AxisDownloadProtocol  protocol;
	gchar                *name;
	gchar                *info_hash;
	gchar                *fetch_ref;	/* secret, wiped */
	gchar                *magnet;	/* secret, wiped */
	gboolean              files_origin;	/* fetch_ref is on the accepted other origin */
} AxisIndexerFetchTarget;

void
axis_indexer_fetch_target_clear(AxisIndexerFetchTarget *target);

/* Fails nothing for a release that is not there: *@found is %FALSE. */
gboolean
axis_indexer_store_load_fetch(
	AxisDbConn              *conn,
	gint64                   release_id,
	gboolean                *found,
	AxisIndexerFetchTarget  *target,
	GError                 **error
);

/* Deletes the searches whose life has ended. */
gboolean
axis_indexer_store_expire(
	AxisDbConn  *conn,
	guint       *n_searches,
	GError     **error
);

/* "123" for SQL parameters. */
gchar *
axis_indexer_int_text(gint64 value);

G_END_DECLS

#endif /* AXIS_INDEXER_PRIVATE_H */
