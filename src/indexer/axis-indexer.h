/*
 * axis-indexer.h - the indexer layer: the indexers and sources of releases,
 * searching them, the releases found, and fetching one
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * To download a title Axis has to find releases of it.  Two ways are
 * configured here, side by side, as rows of one list: a native indexer
 * (Newznab for Usenet, Torznab for torrents), which Axis asks itself with
 * the user's own key, and a source of many indexers (Prowlarr), which
 * answers one search across all of them.  A search with no source named asks
 * every enabled one at once, each within its own limits and deadline, and
 * merges what they found (see axis_indexers_search()).  The promises this
 * layer keeps:
 *
 *  - A search is by identifier where the source takes one and by words
 *    otherwise, and the result says which was used (and when words were used
 *    because the identifier found nothing).  Indexers that take no
 *    identifier are also asked by the title, and the answers merged, each
 *    release once.  Searches are stored with their
 *    releases for a short life (indexers.cache_life, fifteen minutes), keyed
 *    by the normalised request, so that the same search again within that
 *    life asks the source nothing, and a grab refers to a stored release by
 *    its identifier and not by an address from a form.
 *  - A release is fetched only from the source that returned it.  Its fetch
 *    reference is accepted only on the source's own configured origin and
 *    under its base path; a result naming anything else is left out and
 *    counted.  The fetch asks that origin, never an address taken from a
 *    result's text.  The file is size-capped and must be an NZB or a
 *    torrent by its bytes; a redirect to a "magnet:" link is read as that
 *    magnet (checked against the hash the search stated) and never
 *    followed.  A redirect off the origin to the indexer's own site is
 *    followed once, under the HTTP client's rules for it (https, a public
 *    address checked where it is connected to, nothing of Axis's sent:
 *    core/axis-http.h), unless following is switched off for the source;
 *    then it is refused with nothing sent there, and the error says how to
 *    resolve it.  The redirect's address is held for that one request only.
 *  - The source's key is sent only to its origin, in a header; a native
 *    indexer's, as its protocol requires, in the query of the requests to its
 *    origin.  A release's fetch reference holds the key as a parameter, and a
 *    magnet link may hold a tracker's passkey: both are stored as secrets
 *    (own table, never selected by a listing, never rendered), expire with
 *    the search, and are never in a page, a log line, a job row or an error
 *    message.
 *  - A native indexer's file is fetched from its own configured origin, or
 *    from another origin only when an administrator accepted that origin for
 *    that indexer (shown at its test, never accepted by itself, never taken
 *    from a later answer).  A redirect of its download is not followed off
 *    its origin.
 *  - Sources are asked politely: a limit per source (rate and burst), a cap
 *    on calls at once, a pause after a 429, a back-off after failures.  A
 *    native indexer's daily allowances, when it reports them, are shown and
 *    kept to: automatic searches stop short of them, leaving a reserve for
 *    searches a person asks for, and an indexer that says its limit is
 *    reached is not asked again until it has had time to reset.
 *  - Text from an indexer (names above all) is cleaned, cut and only ever
 *    shown as text; a list of results is cut at a thousand.  Nothing is
 *    parsed out of a name here; the release parser's work is not this
 *    layer's.
 *  - What a search says in words is not logged at the default level.
 *
 * The movies and TV modules search through acquire/ (every enabled indexer
 * and source at once) and fetch the release they chose; the administrator's
 * test searches ask one, or all.
 *
 * Threads: everything that touches the database or a source blocks: call it
 * from a worker thread.  Create and free the layer on the main thread.
 */

#ifndef AXIS_INDEXER_H
#define AXIS_INDEXER_H

#include <glib.h>

#include "core/axis-setting.h"
#include "core/axis-settings.h"
#include "db/axis-db-pool.h"
#include "indexer/axis-indexer-kind.h"
#include "indexer/axis-indexer-model.h"
#include "indexer/def/axis-def-trace.h"

G_BEGIN_DECLS

#define AXIS_INDEXER_MODULE_ID           "indexers"
#define AXIS_INDEXER_EXPIRE_KIND         "indexers.expire"
#define AXIS_INDEXER_EXPIRE_SCHEDULE     "indexers.expire"
#define AXIS_INDEXER_EXPIRE_INTERVAL_S   300

/* The native indexers' capabilities are read again by this job when they are
 * older than a day. */
#define AXIS_INDEXER_CAPS_KIND           "indexers.caps"
#define AXIS_INDEXER_CAPS_SCHEDULE       "indexers.caps"
#define AXIS_INDEXER_CAPS_INTERVAL_S     3600
#define AXIS_INDEXER_CAPS_STALE_S        86400

#define AXIS_INDEXER_SETTING_CACHE_LIFE      "indexers.cache_life"
#define AXIS_INDEXER_SETTING_RATE            "indexers.rate"
#define AXIS_INDEXER_SETTING_BURST           "indexers.burst"
#define AXIS_INDEXER_SETTING_MAX_SIMULTANEOUS "indexers.max_simultaneous"

/* Requests a second to each public site (YTS, EZTV, The Pirate Bay), one at
 * a time: half a request a second, one every two seconds, unless changed. */
#define AXIS_INDEXER_SETTING_SITE_RATE       "indexers.site_rate"

/* One request's deadline at a source, and a slow one's (a search across
 * every indexer, a file).  A native indexer's is in indexer/axis-indexer-kind.h. */
#define AXIS_INDEXER_REQUEST_TIMEOUT_MS  15000
#define AXIS_INDEXER_SLOW_TIMEOUT_MS     60000

/* How long a call waits for the source's budget or a free place before it
 * gives up. */
#define AXIS_INDEXER_WAIT_MS             30000

/* The list of a source's indexers is read again at a search when it is older
 * than this. */
#define AXIS_INDEXER_LIST_STALE_S        300

/* At most this many indexers and sources can be configured. */
#define AXIS_INDEXER_MAX_SOURCES         48

/* The priority of an indexer or source when none is given (lower first),
 * and its range. */
#define AXIS_INDEXER_DEFAULT_PRIORITY    25
#define AXIS_INDEXER_MAX_PRIORITY        50

typedef struct _AxisIndexers AxisIndexers;

/**
 * axis_indexers_new:
 * @pool: the database, which must outlive the layer
 * @settings: (nullable): the settings store (%NULL: the defaults)
 *
 * Returns: (transfer full): the layer, reference counted, with no kind yet
 */
AxisIndexers *
axis_indexers_new(
	AxisDbPool   *pool,
	AxisSettings *settings
);

AxisIndexers *
axis_indexers_ref(AxisIndexers *self);

/* The database the layer was made with (for pages that read beside it). */
AxisDbPool *
axis_indexers_get_pool(AxisIndexers *self);

void
axis_indexers_unref(AxisIndexers *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisIndexers, axis_indexers_unref)

/* Cancels requests in flight and refuses new ones (after the job queue has
 * stopped). */
void
axis_indexers_stop(AxisIndexers *self);

/* The module's settings, ended by an entry whose key is %NULL. */
const AxisSettingInfo *
axis_indexers_get_setting_table(void);

/**
 * axis_indexers_add_kind:
 * @info: (transfer none): the kind; it must outlive the layer
 *
 * Before the layer is used.  Fails with %AXIS_INDEXER_ERROR_INVALID when its
 * name is not valid, it lacks a function, a label or a key header, or a kind
 * of that name exists.
 */
gboolean
axis_indexers_add_kind(
	AxisIndexers               *self,
	const AxisIndexerKindInfo  *info,
	GError                    **error
);

guint
axis_indexers_n_kinds(AxisIndexers *self);

const AxisIndexerKindInfo *
axis_indexers_kind_at(
	AxisIndexers *self,
	guint         index
);

/* (nullable) */
const AxisIndexerKindInfo *
axis_indexers_find_kind(
	AxisIndexers *self,
	const gchar  *name
);

/**
 * AxisIndexerTuning:
 * @request_timeout_ms: one request's deadline (0: the default); a slow one's
 *   too, when set
 * @wait_ms: how long a call waits for the budget or a place (0: the default)
 *
 * For tests.
 */
typedef struct
{
	guint request_timeout_ms;
	guint wait_ms;
} AxisIndexerTuning;

void
axis_indexers_set_tuning(
	AxisIndexers            *self,
	const AxisIndexerTuning *tuning
);

/* ── Sources ────────────────────────────────────────────────────── */

/**
 * AxisIndexerSourceInfo:
 * @id, @name, @kind, @address, @accept_untrusted, @enabled: as configured
 * @base_path: the path its own paths are served under ("" for none, else
 *   "/" and segments, no trailing slash); put in front of every request
 * @plain_http: its address is plain http (the page warns)
 * @key_set: a key is stored (never what it is)
 * @state: what the last contact found
 * @state_message: (nullable): the last error, safe to show
 * @version: (nullable): what the source said it is
 * @last_checked_us: the last contact, 0 for never
 * @last_contact_us: the last contact that worked, 0 for never
 * @indexers_us: when the list below was read, 0 for never
 * @indexers: (element-type AxisIndexerInfo): the indexers it reported
 * @n_searches: searches stored now (not yet expired)
 * @follow_redirects: a release whose download the source redirects to the
 *   indexer's own site is fetched from there (on unless switched off;
 *   never for a native indexer)
 * @native: a native indexer (Newznab, Torznab): one indexer, not a source of
 *   many; the fields below are its own
 * @protocol: a native indexer's protocol
 * @priority: lower first: which of two indexers that found the same release
 *   it is fetched from first
 * @movie_categories, @n_movie_categories: the categories a native indexer is
 *   searched in for a movie (Newznab numbers)
 * @tv_categories, @n_tv_categories: and for TV
 * @files_origin: (nullable): the other origin an administrator accepted for
 *   its files ("https://host:port")
 * @files_origin_seen: (nullable): an origin of files its last test saw that
 *   is neither its own nor the accepted one: shown, to accept or not
 * @caps: what its capabilities said, as last read
 * @allowance: its daily allowances, as last reported and counted since
 * @held_until_us: it said its request limit is reached: not searched before
 *   this (0: not held)
 * @grabs_held_until_us: it said its download limit is reached: no file
 *   fetched from it before this
 * @hold_message: (nullable): what it said
 * @host: (nullable): its address's host (axis_indexer_host_key())
 * @definition_id: the installed definition it runs (a kind whose indexers
 *   each run one, %AxisIndexerKindInfo.definition), 0 for none
 * @definition_name: (nullable): that definition's name
 */
typedef struct
{
	gint64                   id;
	gchar                   *name;
	gchar                   *kind;
	gchar                   *address;
	gchar                   *base_path;
	gboolean                 accept_untrusted;
	gboolean                 enabled;
	gboolean                 plain_http;
	gboolean                 key_set;
	AxisIndexerSourceState   state;
	gchar                   *state_message;
	gchar                   *version;
	gint64                   last_checked_us;
	gint64                   last_contact_us;
	gint64                   indexers_us;
	gint64                   created_us;
	gint64                   updated_us;
	GPtrArray               *indexers;
	guint                    n_searches;
	gboolean                 follow_redirects;
	gboolean                 native;
	AxisDownloadProtocol     protocol;
	gint                     priority;
	guint                    n_movie_categories;
	gint                     movie_categories[AXIS_INDEXER_MAX_CATEGORIES];
	guint                    n_tv_categories;
	gint                     tv_categories[AXIS_INDEXER_MAX_CATEGORIES];
	gchar                   *files_origin;
	gchar                   *files_origin_seen;
	AxisIndexerCaps          caps;
	AxisIndexerAllowance     allowance;
	gint64                   held_until_us;
	gint64                   grabs_held_until_us;
	gchar                   *hold_message;
	gchar                   *host;
	gint64                   definition_id;
	gchar                   *definition_name;
} AxisIndexerSourceInfo;

/* Whether automatic searches leave @source out now: a native indexer held
 * after saying its limit is reached, or one whose allowance is within its
 * reserve.  With @automatic %FALSE, only a hold or an allowance used up. */
gboolean
axis_indexer_source_spent(
	const AxisIndexerSourceInfo *source,
	gboolean                     automatic,
	gint64                       now_us
);

/**
 * AxisIndexerHoldCause:
 * @AXIS_INDEXER_HOLD_NONE: not held now
 * @AXIS_INDEXER_HOLD_LIMIT: a native indexer said its request limit is reached
 * @AXIS_INDEXER_HOLD_WAIT: the site asked Axis to wait (a 429, or a 503 with
 *   Retry-After): Axis honours it, a test and a check too, and never lifts it
 * @AXIS_INDEXER_HOLD_DRIFT: Axis held it: its pages no longer look as expected
 * @AXIS_INDEXER_HOLD_CHALLENGE: Axis held it: it answers with a browser
 *   challenge
 *
 * Why searches pass a source over until its %held_until_us.  Only Axis's own
 * holds (drift, a challenge) end when a test or a check passes, or the
 * definition changes.
 */
typedef enum
{
	AXIS_INDEXER_HOLD_NONE,
	AXIS_INDEXER_HOLD_LIMIT,
	AXIS_INDEXER_HOLD_WAIT,
	AXIS_INDEXER_HOLD_DRIFT,
	AXIS_INDEXER_HOLD_CHALLENGE
} AxisIndexerHoldCause;

/* How a hold of a source that runs a definition is worded: its name, then
 * one of these, which says the cause.  A message that ends with none of the
 * two of Axis's is the site's. */
#define AXIS_INDEXER_HOLD_WAIT_WORDS      " asked Axis to wait"
#define AXIS_INDEXER_HOLD_DRIFT_WORDS     "'s pages no longer look as expected"
#define AXIS_INDEXER_HOLD_CHALLENGE_WORDS " answers with a browser challenge Axis cannot pass"

AxisIndexerHoldCause
axis_indexer_hold_cause(
	const AxisIndexerSourceInfo *source,
	gint64                       now_us
);

/* Whether some indexer of @source that is on, of @protocol, redirects its
 * downloads to its own site, and following is switched off for @source:
 * those releases cannot be fetched. */
gboolean
axis_indexer_source_redirects_refused(
	const AxisIndexerSourceInfo *source,
	AxisDownloadProtocol         protocol
);

/* Whether every indexer of @source that is on, of @protocol, is in that
 * case (and there is at least one). */
gboolean
axis_indexer_source_all_redirects_refused(
	const AxisIndexerSourceInfo *source,
	AxisDownloadProtocol         protocol
);

void
axis_indexer_source_info_free(AxisIndexerSourceInfo *source);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisIndexerSourceInfo, axis_indexer_source_info_free)

/* Every source, by name. */
gboolean
axis_indexers_list_sources(
	AxisIndexers  *self,
	GPtrArray    **sources,
	GError       **error
);

/* Fails with %AXIS_INDEXER_ERROR_NOT_FOUND. */
gboolean
axis_indexers_get_source(
	AxisIndexers            *self,
	gint64                   id,
	AxisIndexerSourceInfo  **source,
	GError                 **error
);

/**
 * AxisIndexerSourceSpec:
 * @name: as typed
 * @kind: a registered kind's name (only for a new source; a source's kind
 *   does not change)
 * @address: as typed: an http or https origin
 * @base_path: (nullable): as typed (empty or %NULL: none); checked by
 *   axis_download_base_path_normalize() and stored without a trailing slash
 * @accept_untrusted: accept a certificate that does not verify
 * @enabled: whether it is used
 * @secret: (nullable): a new key; %NULL or "" keeps the stored one (an
 *   edit) or sets none (a new source)
 * @refuse_redirects: do not follow a download the source redirects to the
 *   indexer's own site.  %FALSE (following, the default) unless the
 *   administrator switched it off: a spec zeroed and filled with the other
 *   fields keeps the default.  Not used for a native indexer, which never
 *   follows one
 * @priority: 1 to %AXIS_INDEXER_MAX_PRIORITY, lower first; 0 for
 *   %AXIS_INDEXER_DEFAULT_PRIORITY
 * @movie_categories, @tv_categories: (nullable): a native indexer's, as
 *   typed: Newznab category numbers separated by commas or spaces ("2000,
 *   2040"); %NULL or empty for the defaults (2000; 5000)
 * @files_origin: (nullable): a native indexer's other origin for its files,
 *   as typed ("https://files.example"); %NULL or empty for none.  Typing it
 *   is accepting it
 * @definition_id: for a new indexer of a kind whose indexers each run a
 *   definition: the installed definition, which must be usable (it does not
 *   change later).  Its address is one of the definition's links or one
 *   typed, an http or https address that may have a path (kept as the base
 *   path); empty is the definition's first link.  0 for any other kind
 *
 * What an administrator submitted.
 */
typedef struct
{
	const gchar *name;
	const gchar *kind;
	const gchar *address;
	const gchar *base_path;
	gboolean     accept_untrusted;
	gboolean     enabled;
	const gchar *secret;
	gboolean     refuse_redirects;
	gint         priority;
	const gchar *movie_categories;
	const gchar *tv_categories;
	const gchar *files_origin;
	gint64       definition_id;
} AxisIndexerSourceSpec;

/**
 * axis_indexers_save_source:
 * @id: the source to change, or 0 for a new one
 * @id_out: (out) (optional): its id
 *
 * Fails with %AXIS_INDEXER_ERROR_INVALID (the message says which field,
 * without its value), %AXIS_INDEXER_ERROR_EXISTS for a name in use, and
 * %AXIS_INDEXER_ERROR_NOT_FOUND.  A changed address, base path or key forgets
 * the source's stored searches (they were for another place) and its list of
 * indexers.
 */
gboolean
axis_indexers_save_source(
	AxisIndexers                 *self,
	gint64                        id,
	const AxisIndexerSourceSpec  *spec,
	gint64                       *id_out,
	GError                      **error
);

/* Forgets a source, its key, its indexers and its stored searches. */
gboolean
axis_indexers_delete_source(
	AxisIndexers  *self,
	gint64         id,
	GError       **error
);

/**
 * axis_indexers_test_source:
 * @id: the source
 * @cancellable: (nullable)
 *
 * Reaches the source, authenticates, and reads its version and its indexers,
 * storing what was found on the source (its state, version, message, and
 * the list of indexers) whatever it was.  Fails only for a database error,
 * cancellation or %AXIS_INDEXER_ERROR_NOT_FOUND: a source that does not
 * answer is a result, not a failure; read it with axis_indexers_get_source().
 */
gboolean
axis_indexers_test_source(
	AxisIndexers  *self,
	gint64         id,
	GCancellable  *cancellable,
	GError       **error
);

/**
 * axis_indexers_check_source:
 * @id: an indexer that runs a site definition
 * @words: what to search for, as typed
 * @excerpts: record a bounded excerpt of each traced row (and of a page no
 *   row was read from)
 * @cancellable: (nullable)
 * @trace: (out) (transfer full): the account of the search, step by step
 *   (indexer/def/axis-def-trace.h); set whenever the check was made, the
 *   site failing or not
 *
 * One search by @words through the indexer's own connection: its address,
 * its rate and its back-off, as a search's (a hold for drift is not one
 * that stops it), every request and answer recorded.  Nothing found is
 * stored; the indexer's state is, as a test's: a check that reads its
 * answers lifts a hold, one that fails says why.  Fails, with no trace, for
 * words that are empty, an indexer that does not run a definition, one that
 * does not exist, cancellation or a database error.
 */
gboolean
axis_indexers_check_source(
	AxisIndexers   *self,
	gint64          id,
	const gchar    *words,
	gboolean        excerpts,
	GCancellable   *cancellable,
	AxisDefTrace  **trace,
	GError        **error
);

/**
 * axis_indexers_accept_files_origin:
 * @id: a native indexer
 *
 * Accepts, for that indexer's files, the other origin its last test saw
 * (%AxisIndexerSourceInfo.files_origin_seen), and forgets its stored
 * searches (they left those results out).  Fails with
 * %AXIS_INDEXER_ERROR_NOT_FOUND, and %AXIS_INDEXER_ERROR_INVALID when there
 * is none to accept.
 */
gboolean
axis_indexers_accept_files_origin(
	AxisIndexers  *self,
	gint64         id,
	GError       **error
);

/**
 * axis_indexers_refresh_caps:
 * @n_read: (out) (optional): how many were read
 *
 * Reads again the capabilities of every enabled native indexer whose
 * capabilities are older than %AXIS_INDEXER_CAPS_STALE_S (the job
 * "indexers.caps").  An indexer that does not answer is recorded as a
 * failed contact, not as a failure of this.
 */
gboolean
axis_indexers_refresh_caps(
	AxisIndexers  *self,
	GCancellable  *cancellable,
	guint         *n_read,
	GError       **error
);

/* ── Installed definitions ──────────────────────────────────────── */

/*
 * Indexer definitions (the community's "Cardigann" YAML files, indexer/def/
 * axis-def.h).  Axis ships a small set it maintains and tests
 * (indexer/axis-indexer-shipped.h: their ids begin with "axis-"), written to
 * the database at every start; any other is the administrator's data,
 * installed only when an administrator gives Axis a file.  A built-in one
 * can be overridden by the administrator with a newer version of it, kept
 * apart and used while it is newer.  Each is judged again whenever it is
 * read (so that what an indexer runs is what this version of the engine can
 * run).  Axis fetches none by itself; an administrator can fetch the ones
 * they choose from Jackett's repository (below: "Definitions fetched from
 * Jackett").
 */

/* The largest definition file installed, in bytes. */
#define AXIS_INDEXER_DEFINITION_MAX_BYTES (256 * 1024)

/* At most this many definitions are installed. */
#define AXIS_INDEXER_MAX_DEFINITIONS      1000

/**
 * AxisIndexerDefinitionInfo:
 * @id: the stored definition
 * @def_id: the definition's own id ("tidepool")
 * @name, @description, @language, @site_type: as it says, cleaned
 * @links: (array zero-terminated=1): its site's addresses, the preferred
 *   first, then the ones it lists as old
 * @usable: every feature it needs is supported
 * @problem: (nullable): why not, a sentence ("Needs a login (stage 2).")
 * @movies, @tv, @general: what it can be searched for (its categories)
 * @size_bytes: the file's size
 * @sha256: the file's SHA-256 in hex
 * @installed_us, @updated_us: when it was first installed, and replaced
 * @n_indexers: how many indexers run it
 * @built_in: it is one Axis ships (what is above is of the file in use: the
 *   built-in copy, or the override when that is used)
 * @version: the version of the file in use (0 when it states none)
 * @built_in_version: a built-in one's version as this Axis ships it
 * @override_version: a built-in one's stored override's version (0 for none)
 * @overridden: the override is in use
 * @override_ignored: there is an override, and it is not used
 * @override_note: (nullable): why it is not used, a sentence
 * @override_us: when the override was stored (0 for none)
 * @origin: (nullable): "jackett" for a file fetched from Jackett's
 *   repository; %NULL for one installed by hand (or built in)
 * @origin_file: (nullable): its file's name there ("madeupsite.yml")
 * @origin_commit: (nullable): the commit it was read at
 * @origin_us: when it was fetched (0 for none)
 * @edited: fetched, and the file stored is no longer the one fetched (one
 *   was installed over it by hand)
 * @has_previous: the file a fetch replaced is kept and can be put back
 * @newer_upstream: the list last read from Jackett has another file for it
 * @gone_upstream: a list was read from Jackett since, and it does not hold
 *   this file's name
 * @previous_from_jackett: the kept earlier file is one fetched from Jackett as
 *   fetched (not installed by hand, nor changed by hand since)
 * @previous_us: when the kept earlier file was fetched (0 when it was not)
 *
 * A definition, as judged now.
 */
typedef struct
{
	gint64     id;
	gchar     *def_id;
	gchar     *name;
	gchar     *description;
	gchar     *language;
	gchar     *site_type;
	gchar    **links;
	gboolean   usable;
	gchar     *problem;
	gboolean   movies;
	gboolean   tv;
	gboolean   general;
	gint64     size_bytes;
	gchar     *sha256;
	gint64     installed_us;
	gint64     updated_us;
	guint      n_indexers;
	gboolean   built_in;
	gint       version;
	gint       built_in_version;
	gint       override_version;
	gboolean   overridden;
	gboolean   override_ignored;
	gchar     *override_note;
	gint64     override_us;
	gchar     *origin;
	gchar     *origin_file;
	gchar     *origin_commit;
	gint64     origin_us;
	gboolean   edited;
	gboolean   has_previous;
	gboolean   newer_upstream;
	gboolean   gone_upstream;
	gboolean   previous_from_jackett;
	gint64     previous_us;
} AxisIndexerDefinitionInfo;

void
axis_indexer_definition_info_free(AxisIndexerDefinitionInfo *info);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisIndexerDefinitionInfo, axis_indexer_definition_info_free)

/**
 * AxisIndexerDefinitionInstalled:
 * @id: the stored definition
 * @replaced: one of the same id was installed and is replaced
 * @unchanged: it was, with the same content (nothing changed)
 * @was_usable: what the replaced one was
 * @override: it was stored as the override of a built-in definition (@id is
 *   the built-in one's)
 *
 * What an install did.
 */
typedef struct
{
	gint64   id;
	gboolean replaced;
	gboolean unchanged;
	gboolean was_usable;
	gboolean override;
} AxisIndexerDefinitionInstalled;

/**
 * axis_indexers_install_definition:
 * @data: the file, as uploaded
 * @length: its length
 *
 * Reads and judges the file and stores it, replacing an installed one of
 * the same id.  A file that is not a definition (or larger than
 * %AXIS_INDEXER_DEFINITION_MAX_BYTES) fails with %AXIS_INDEXER_ERROR_INVALID,
 * saying why, and nothing is stored; one that is not usable is stored, and
 * listed with its reason.  Indexers that run the replaced one run the new
 * one from their next call, and their stored searches are forgotten.
 *
 * A file with the id of a built-in definition is that definition's
 * override: it is stored only when axis_indexer_shipped_check_override()
 * accepts it (usable, newer, its requests built), else refused saying why;
 * the holds of the indexers that run it are lifted.  Any other id beginning
 * with "axis-" is refused: those are Axis's own.
 */
gboolean
axis_indexers_install_definition(
	AxisIndexers                    *self,
	const gchar                     *data,
	gsize                            length,
	AxisIndexerDefinitionInstalled  *installed,
	GError                         **error
);

/* The file the indexers that run the definition @id run now (a built-in
 * one's copy or its override): text from the database or the daemon, to be
 * shown as text.  Fails with %AXIS_INDEXER_ERROR_NOT_FOUND. */
gboolean
axis_indexers_get_definition_text(
	AxisIndexers  *self,
	gint64         id,
	gchar        **text,
	GError       **error
);

/**
 * axis_indexers_remove_definition_override:
 * @id: a built-in definition
 *
 * Forgets its override: the indexers that run it run the built-in copy from
 * their next call, their kept searches forgotten and their holds lifted.
 * Fails with %AXIS_INDEXER_ERROR_INVALID when it has none.
 */
gboolean
axis_indexers_remove_definition_override(
	AxisIndexers  *self,
	gint64         id,
	GError       **error
);

/**
 * axis_indexers_sync_shipped:
 * @definitions_dir: (nullable): the data directory's "definitions", where an
 *   administrator may place an override as "<id>.yml"
 *
 * Writes every built-in definition's row (inserted when missing, its file
 * replaced when it differs, the kept searches of the indexers that run it
 * forgotten then), and installs each override found in @definitions_dir that
 * differs from the one stored, through the same checks as an upload; one
 * refused is logged with why and changes nothing.  Run at every start.
 */
gboolean
axis_indexers_sync_shipped(
	AxisIndexers  *self,
	const gchar   *definitions_dir,
	GError       **error
);

/* Every definition (the built-in ones among them; an override is shown on
 * its built-in one), by name, judged now. */
gboolean
axis_indexers_list_definitions(
	AxisIndexers  *self,
	GPtrArray    **definitions,
	GError       **error
);

/* Fails with %AXIS_INDEXER_ERROR_NOT_FOUND. */
gboolean
axis_indexers_get_definition(
	AxisIndexers               *self,
	gint64                      id,
	AxisIndexerDefinitionInfo **definition,
	GError                    **error
);

/* Forgets an installed definition; refused (%AXIS_INDEXER_ERROR_INVALID)
 * while an indexer runs it. */
gboolean
axis_indexers_remove_definition(
	AxisIndexers  *self,
	gint64         id,
	GError       **error
);

/* ── Definitions fetched from Jackett ───────────────────────────── */

/*
 * add-definition-fetch.  Axis ships none of the community's definitions and
 * fetches nothing by itself: no request below is made at start, by a job or
 * by a schedule.  An administrator asks for the list of Jackett's definition
 * files (axis_indexers_jackett_refresh(): two requests to GitHub's API, the
 * commit of the branch and the folder's files at that commit, kept in the
 * database) and then for the files they choose (axis_indexers_jackett_
 * fetch(): each from raw.githubusercontent.com at that same commit, checked
 * to be the file listed by its git blob id, judged exactly as a file
 * installed by hand, and stored as one, with where it came from).
 *
 * Requests go only to the two fixed https hosts, through core/axis-http.h
 * (GET only, no redirect to another host, size and time limits, the outbound
 * binding as for an indexer: %AXIS_OUTBOUND_TRAFFIC_DEFINITIONS).  A file's
 * name is accepted only as "[a-z0-9][a-z0-9-]*\.yml" and must be in the list
 * last read.  Nothing is enabled or searched by a fetch: an indexer is added
 * from a definition as before.
 */

/* The origin recorded for a fetched definition. */
#define AXIS_INDEXER_ORIGIN_JACKETT        "jackett"

/* Where the files are, for the pages to say. */
#define AXIS_INDEXER_JACKETT_REPOSITORY    "github.com/Jackett/Jackett"
#define AXIS_INDEXER_JACKETT_FOLDER        "src/Jackett.Common/Definitions"
#define AXIS_INDEXER_JACKETT_BRANCH        "master"

/* At most this many files are fetched by one request of a person. */
#define AXIS_INDEXER_JACKETT_MAX_PER_FETCH 50

/* All of one fetch's requests end within this long; what is left is said
 * not to have been fetched. */
#define AXIS_INDEXER_JACKETT_FETCH_SECONDS 120

/**
 * AxisIndexerJackettEntry:
 * @name: the file's name ("madeupsite.yml")
 * @size_bytes: its size as listed
 * @definition_id: the installed definition of that id (the name without
 *   ".yml"), 0 when none is installed
 * @definition_name: (nullable): the installed one's name
 * @differs: the installed one's file is not the listed one
 * @from_jackett: the installed one was fetched from Jackett
 * @by_hand: the installed one was installed by hand, or changed by hand
 *   since it was fetched: a fetch does not replace it without the tick
 * @n_indexers: how many configured indexers run the installed one
 */
typedef struct
{
	gchar    *name;
	gint64    size_bytes;
	gint64    definition_id;
	gchar    *definition_name;
	gboolean  differs;
	gboolean  from_jackett;
	gboolean  by_hand;
	guint     n_indexers;
} AxisIndexerJackettEntry;

/**
 * AxisIndexerJackettListing:
 * @commit: the commit the list was read at
 * @listed_us: when
 * @n_files: how many files the list holds
 * @n_left_out: entries of the folder that were not taken (not a file, or a
 *   name of another form)
 * @cut: GitHub may have cut the list (it holds as many as GitHub gives)
 * @entries: (element-type AxisIndexerJackettEntry): the files asked for, by
 *   name
 */
typedef struct
{
	gchar     *commit;
	gint64     listed_us;
	guint      n_files;
	guint      n_left_out;
	gboolean   cut;
	GPtrArray *entries;
} AxisIndexerJackettListing;

void
axis_indexer_jackett_listing_free(AxisIndexerJackettListing *listing);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisIndexerJackettListing, axis_indexer_jackett_listing_free)

/**
 * axis_indexers_jackett_refresh:
 * @n_files: (out) (optional): how many files the new list holds
 *
 * Asks GitHub for the list and stores it in place of the one before.  Fails
 * with %AXIS_INDEXER_ERROR_RATE_LIMITED when GitHub refuses for now (the
 * message says so plainly), %AXIS_INDEXER_ERROR_UNREADABLE for an answer
 * that is not a list, %AXIS_INDEXER_ERROR_BUSY while another asking or fetch
 * runs; the list kept is then the one before.  Blocking: two requests.
 */
gboolean
axis_indexers_jackett_refresh(
	AxisIndexers  *self,
	GCancellable  *cancellable,
	guint         *n_files,
	GError       **error
);

/**
 * axis_indexers_jackett_get_listing:
 * @words: (nullable): only the files whose name holds this (lower case)
 * @listing: (out) (transfer full) (nullable): the list kept, with what is
 *   installed of it; %NULL when none was ever read
 *
 * Reads the database only: nothing is asked of GitHub.
 */
gboolean
axis_indexers_jackett_get_listing(
	AxisIndexers               *self,
	const gchar                *words,
	AxisIndexerJackettListing **listing,
	GError                    **error
);

typedef enum
{
	AXIS_INDEXER_JACKETT_INSTALLED,	/* new on this Axis */
	AXIS_INDEXER_JACKETT_UPDATED,	/* replaced the one installed, which is kept to put back */
	AXIS_INDEXER_JACKETT_UNCHANGED,	/* the file installed already */
	AXIS_INDEXER_JACKETT_REFUSED	/* not installed: @message says why */
} AxisIndexerJackettResult;

typedef struct
{
	gchar                    *name;
	AxisIndexerJackettResult  result;
	gchar                    *message;	/* nullable: why it was refused, or a note on what was done */
	gint64                    definition_id;	/* 0 when refused */
	guint                     n_indexers;	/* configured indexers that run the definition (when it was installed already) */
	gboolean                  cannot_run;	/* updated to a file Axis cannot run (a nonzero @n_indexers will not search) */
} AxisIndexerJackettOutcome;

void
axis_indexer_jackett_outcome_free(AxisIndexerJackettOutcome *outcome);

/**
 * axis_indexers_jackett_fetch:
 * @names: (array zero-terminated=1): the files' names, 1 to
 *   %AXIS_INDEXER_JACKETT_MAX_PER_FETCH
 * @replace_by_hand: also replace a definition installed by hand or changed
 *   by hand since it was fetched (without it such a one is refused, saying
 *   so)
 * @outcomes: (out) (transfer full) (element-type AxisIndexerJackettOutcome):
 *   what became of each name, in order
 *
 * Downloads each file at the commit of the list kept and installs it.  A
 * name that is not of the accepted form or not in the list, a file larger
 * than %AXIS_INDEXER_DEFINITION_MAX_BYTES, one whose bytes are not the file
 * listed (its git blob id), one that is not a definition or whose id is not
 * its file's name, and one with a built-in definition's id are refused, each
 * with its reason, and nothing of it is stored.  One that is a definition
 * but not usable yet is stored and listed with why, as a file installed by
 * hand is.  An update to a file Axis cannot run, of a definition that
 * indexers use, is refused unless @replace_by_hand.  A database error on a
 * file is that file's refusal, and the files after it are not asked for: the
 * outcomes of those before are still returned.
 *
 * Fails as a whole (nothing asked) with %AXIS_INDEXER_ERROR_INVALID for no
 * names, too many, or no list read yet, and %AXIS_INDEXER_ERROR_BUSY while
 * another fetch runs.
 */
gboolean
axis_indexers_jackett_fetch(
	AxisIndexers        *self,
	const gchar *const  *names,
	gboolean             replace_by_hand,
	GCancellable        *cancellable,
	GPtrArray          **outcomes,
	GError             **error
);

/**
 * axis_indexers_jackett_put_back:
 * @id: a definition a fetch replaced
 *
 * Puts the file the last fetch replaced back in use, with where it came
 * from; the file it takes the place of is not kept.  The indexers that run
 * the definition run the earlier file from their next call.  Fails with
 * %AXIS_INDEXER_ERROR_INVALID when no earlier file is kept.
 */
gboolean
axis_indexers_jackett_put_back(
	AxisIndexers  *self,
	gint64         id,
	GError       **error
);

/**
 * axis_indexers_jackett_set_testing:
 * @api_base: (nullable): the origin asked in place of https://api.github.com
 * @raw_base: (nullable): in place of https://raw.githubusercontent.com
 *
 * FOR TESTS ONLY: the stand-in for GitHub listens on loopback.  Nothing in
 * the daemon calls this and no setting reaches it (a test reads the sources
 * to prove it).  An origin whose host is not 127.0.0.1 is not taken (the
 * call changes nothing).  %NULL and %NULL restore the fixed hosts.
 */
void
axis_indexers_jackett_set_testing(
	const gchar *api_base,
	const gchar *raw_base
);

/* ── Searching and fetching ─────────────────────────────────────── */

/**
 * AxisIndexerSearchResult:
 * @search_id: the stored search
 * @source_id: the source asked
 * @mode: how it was searched
 * @fell_back: an identifier search found nothing and words were used
 * @cached: the stored result of an earlier identical search, within its life:
 *   the source was not asked
 * @truncated: there were more results than a search keeps
 * @n_off_origin: results left out because they named a fetch address that is
 *   not the source's own
 * @n_unusable: results left out because there was no way to fetch them
 * @n_skipped: results left out because they could not be read
 * @n_indexers: how many of the source's indexers were on, -1 when not known
 * @failed: (nullable): the names of those it said were failing, a
 *   %NULL-ended vector, or %NULL for none known
 * @created_us, @expires_us: when it was made and when it is forgotten
 * @releases: (element-type AxisIndexerRelease): the releases, in the order
 *   the source gave them, each with its stored identifier
 * @also_text: the indexers that take no identifier were also asked by the
 *   title, and the answers merged (@mode is the identifier searched by)
 * @parts: (element-type AxisIndexerSearchPart) (nullable): of a search of
 *   every indexer and source (no source named), one for each, asked or
 *   passed over, by priority: what each found or why it gave nothing
 * @n_merged: releases left out because another indexer found the same one
 *   (they are its alternatives)
 * @not_asked: (nullable): of one indexer, why it was not asked at all: it has
 *   nothing for this kind of search (see %AxisIndexerOutcome.not_asked).  Such
 *   a result is made at the time, holds no releases and is never stored
 * @asked_directly: (element-type utf8) (nullable): of a search of every
 *   indexer and source, one sentence ("NewsOwl is asked directly, not through
 *   Prowlarr", no full stop) for each indexer a source also lists that was
 *   asked directly and left out of the source's search
 *
 * What a search found.  Of a search of every indexer and source, @search_id
 * and @source_id are those of the one part when only one was asked, else 0;
 * @mode, @fell_back and @also_text the first part's that answered; @cached
 * whether every part that answered did so from its stored search; the counts
 * the sums of the parts'.
 */
typedef struct
{
	gint64            search_id;
	gint64            source_id;
	AxisIndexerMode   mode;
	gboolean          fell_back;
	gboolean          cached;
	gboolean          truncated;
	guint             n_off_origin;
	guint             n_unusable;
	guint             n_skipped;
	gint              n_indexers;
	gchar           **failed;
	gint64            created_us;
	gint64            expires_us;
	GPtrArray        *releases;
	gboolean          also_text;
	GPtrArray        *parts;
	guint             n_merged;
	gchar            *not_asked;
	GPtrArray        *asked_directly;
} AxisIndexerSearchResult;

void
axis_indexer_search_result_free(AxisIndexerSearchResult *result);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisIndexerSearchResult, axis_indexer_search_result_free)

/**
 * AxisIndexerSearchPart:
 * @source_id, @name, @kind, @native, @priority: the indexer or source
 * @asked: it was asked (or answered from its stored search); %FALSE when it
 *   was passed over (@problem says why) or had nothing for this kind of search
 *   (@not_asked says so: no request was made, and it did not "answer")
 * @not_asked: (nullable): why it was not asked, in words that follow "Not
 *   asked:" ("it has no movies"); not a problem, so @problem is then %NULL
 * @mode, @fell_back, @also_text, @cached: how it was searched
 * @n_releases: what it found
 * @problem: (nullable): why it gave nothing: its error (safe to show, naming
 *   it and never its key or an address), or why it was passed over
 *
 * One indexer's or source's share of a search of all of them.
 */
typedef struct
{
	gint64           source_id;
	gchar           *name;
	gchar           *kind;
	gboolean         native;
	gint             priority;
	gboolean         asked;
	AxisIndexerMode  mode;
	gboolean         fell_back;
	gboolean         also_text;
	gboolean         cached;
	guint            n_releases;
	gchar           *problem;
	gchar           *not_asked;
} AxisIndexerSearchPart;

void
axis_indexer_search_part_free(AxisIndexerSearchPart *part);

/**
 * axis_indexers_search:
 * @source_id: the indexer or source, or 0 for every enabled one
 * @request: what to find; see axis_indexer_request_check()
 * @cancellable: (nullable)
 * @result: (out) (transfer full): the releases, stored
 *
 * One indexer or source: returns the stored result of the same request when
 * it is within its life; otherwise asks it (within its limit; waiting for it
 * at most a while), stores the releases and returns them.  Two identical
 * searches at once ask it once.
 *
 * Every one (@source_id 0): each enabled native indexer and source is
 * searched as above, all at once, each under its own limiter and deadline,
 * so that one that is slow or fails delays the others by nothing beyond its
 * own deadline.  A native indexer near its allowance (for an automatic
 * search) or held after saying its limit is reached is passed over; a
 * source's indexer that is also configured natively (the same host) is not
 * asked through the source.  The answers are merged by priority: a release
 * found by several indexers (the same info-hash; for Usenet the same exact
 * name and size) is returned once, from the first, and the others are its
 * alternatives (%AxisIndexerRelease.also_at), which axis_indexers_fetch()
 * tries when the first cannot be fetched.  What each found or why it failed
 * is in @result's parts; the search fails only when none answered.
 *
 * Fails with %AXIS_INDEXER_ERROR_INVALID for a request that cannot be made,
 * %AXIS_INDEXER_ERROR_NO_SOURCE or _NOT_FOUND, and with the source's error
 * (the source's state is recorded too; of a search of every one, the first
 * one's error when none answered).
 */
gboolean
axis_indexers_search(
	AxisIndexers              *self,
	gint64                     source_id,
	const AxisIndexerRequest  *request,
	GCancellable              *cancellable,
	AxisIndexerSearchResult  **result,
	GError                   **error
);

/**
 * axis_indexers_get_release:
 * @release_id: a stored release's identifier
 *
 * The release as it was returned (no fetch reference).  Fails with
 * %AXIS_INDEXER_ERROR_NOT_FOUND when there is none or its search has expired.
 */
gboolean
axis_indexers_get_release(
	AxisIndexers         *self,
	gint64                release_id,
	AxisIndexerRelease  **release,
	GError              **error
);

/**
 * axis_indexers_fetch:
 * @release_id: a stored release's identifier
 * @cancellable: (nullable)
 * @fetched: (out): the file or the magnet link; clear it with
 *   axis_indexer_fetched_clear()
 *
 * Fetches the release from the source that returned it (see the promises
 * above); when that fails for anything but a cancellation, from each other
 * indexer that found the same release in a search that has not expired, by
 * priority, before it fails (@fetched says which gave it).  Fails with
 * %AXIS_INDEXER_ERROR_NOT_FOUND for no such release or an expired search,
 * and with the first source's error (saying how many others were tried);
 * %AXIS_INDEXER_ERROR_REFUSED for content that is neither an NZB nor a
 * torrent (or the wrong one for the release's protocol), or a redirect to
 * something that is not a valid magnet link, and
 * %AXIS_INDEXER_ERROR_TOO_LARGE for a file over %AXIS_INDEXER_MAX_FILE_BYTES.
 */
gboolean
axis_indexers_fetch(
	AxisIndexers        *self,
	gint64               release_id,
	GCancellable        *cancellable,
	AxisIndexerFetched  *fetched,
	GError             **error
);

/**
 * axis_indexers_expire:
 * @n_searches: (out) (optional): how many searches were forgotten
 *
 * Forgets the searches whose life has ended, with their releases and the
 * secrets of those.  The job "indexers.expire" runs this.
 */
gboolean
axis_indexers_expire(
	AxisIndexers  *self,
	guint         *n_searches,
	GError       **error
);

G_END_DECLS

#endif /* AXIS_INDEXER_H */
