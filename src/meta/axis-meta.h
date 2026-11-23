/*
 * axis-meta.h - the metadata layer: what exists (films, series, their
 * seasons and episodes, their identifiers), from providers, kept locally
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Providers (meta/axis-meta-provider.h) are spoken to directly with the
 * user's own keys.  Every answer used is kept (0018_metadata.sql), and
 * pages and jobs ask this layer, which answers from the store and says how
 * old the answer is:
 *
 *  - An answer missing or older than its freshness limit is fetched again:
 *    the question is queued for that provider's own fetch threads, and the
 *    caller waits for it at most @wait_ms (an interactive search: a few
 *    seconds), then gets what the store has, with the reason the provider
 *    did not answer.  A provider being down is never an error here: the
 *    saved answer is shown, with the time it was fetched.  A fetch that
 *    outlives its waiter goes on and is stored when it ends.
 *  - The same question asked twice at once is one fetch.
 *  - Identity is the external identifier, never the text: the same title
 *    found by a search and by an identifier is one record, and a title
 *    fetched again is updated in place.  An answer that cannot be read is
 *    a failure of that request and changes nothing stored.
 *  - Freshness (settings, metadata.fresh_*): searches a day; a film not yet
 *    released or a series still running a day; a released film thirty
 *    days; an ended series ninety; the current season's episodes a day
 *    (other seasons' as a released film); negative answers a day.
 *  - The refresh job (meta.refresh, every 15 minutes) works through what is
 *    stale, oldest first, within each provider's rate limit, only for
 *    titles something refers to: viewed within metadata.viewed_window, or
 *    named by a reference a module added (axis_meta_add_reference()).
 *  - Artwork is fetched once from the provider's image origin, checked
 *    (meta/axis-meta-image.h: JPEG, PNG or WebP, within the size and
 *    dimension limits, its declared type the one its bytes show), stored in
 *    the data directory under its content hash and served by the daemon at
 *    /art/<hash>.  A cleanup job (meta.artwork.cleanup, hourly) removes what
 *    no title refers to and keeps the total under metadata.artwork_cap,
 *    least recently used first.
 *
 * What leaves the machine: the search text or a title's identifier, the
 * key (in a header, to its provider's API origin only) and a fixed user
 * agent naming Axis and its version.  Search text is never logged.
 *
 * Threads: the reads, the jobs and the artwork functions block on the
 * database and the network: call them from a worker thread.  Create, start
 * and stop the layer on the main thread.
 */

#ifndef AXIS_META_H
#define AXIS_META_H

#include <glib.h>

#include "core/axis-data-dir.h"
#include "core/axis-settings.h"
#include "db/axis-db-pool.h"
#include "jobs/axis-job-context.h"
#include "meta/axis-meta-limit.h"
#include "meta/axis-meta-model.h"
#include "meta/axis-meta-provider.h"

G_BEGIN_DECLS

#define AXIS_META_MODULE_ID           "metadata"
#define AXIS_META_QUEUE               "metadata"
#define AXIS_META_REFRESH_KIND        "meta.refresh"
#define AXIS_META_CLEANUP_KIND        "meta.artwork.cleanup"
#define AXIS_META_REFRESH_SCHEDULE    "meta.refresh"
#define AXIS_META_CLEANUP_SCHEDULE    "meta.artwork.cleanup"
#define AXIS_META_REFRESH_INTERVAL_S  900
#define AXIS_META_CLEANUP_INTERVAL_S  3600

#define AXIS_META_SETTING_FRESH_SEARCH     "metadata.fresh_search"
#define AXIS_META_SETTING_FRESH_UNRELEASED "metadata.fresh_unreleased"
#define AXIS_META_SETTING_FRESH_RELEASED   "metadata.fresh_released"
#define AXIS_META_SETTING_FRESH_ENDED      "metadata.fresh_ended"
#define AXIS_META_SETTING_FRESH_SEASON     "metadata.fresh_current_season"
#define AXIS_META_SETTING_FRESH_NEGATIVE   "metadata.fresh_negative"
#define AXIS_META_SETTING_VIEWED_WINDOW    "metadata.viewed_window"
#define AXIS_META_SETTING_ARTWORK_CAP      "metadata.artwork_cap"

/* How long an interactive question waits for its provider by default. */
#define AXIS_META_DEFAULT_WAIT_MS     4000

/* One request's deadline at the provider. */
#define AXIS_META_REQUEST_TIMEOUT_MS  10000

/* The largest answer read from a provider's API. */
#define AXIS_META_MAX_ANSWER_BYTES    (2 * 1024 * 1024)

/* Artwork is refused beyond these. */
#define AXIS_META_ART_MAX_BYTES       (4 * 1024 * 1024)
#define AXIS_META_ART_MAX_SIDE        10000
#define AXIS_META_ART_MAX_PIXELS      40000000

/* At most this many titles and seasons of one provider per refresh run. */
#define AXIS_META_REFRESH_BATCH       200

/* Saved searches are dropped this long after they were fetched. */
#define AXIS_META_SEARCH_KEEP_S       (30 * 24 * 3600)

typedef struct _AxisMeta AxisMeta;

/**
 * axis_meta_new:
 * @pool: the database, which must outlive the layer
 * @settings: (nullable): the settings store (%NULL: defaults, and no
 *   provider has a key)
 * @data: (nullable): the data directory; %NULL keeps no artwork
 * @error: (nullable): an %AXIS_ERROR_DATA_DIR when the artwork directory
 *   cannot be made in @data (the message names it)
 *
 * Returns: (transfer full) (nullable): the layer, reference counted; it has
 *   no provider yet
 */
AxisMeta *
axis_meta_new(
	AxisDbPool    *pool,
	AxisSettings  *settings,
	AxisDataDir   *data,
	GError       **error
);

AxisMeta *
axis_meta_ref(AxisMeta *self);

void
axis_meta_unref(AxisMeta *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisMeta, axis_meta_unref)

/**
 * axis_meta_add_provider:
 * @info: (transfer none): the provider; it must outlive the layer (a static
 *   table, normally)
 *
 * Adds a provider, before the module is registered (its key and rate
 * settings are part of the module's settings).  Fails with
 * %AXIS_META_ERROR_INVALID when its name or an origin is not valid, it
 * serves no kind, lacks a function, or a provider of that name exists.
 *
 * Returns: %TRUE on success
 */
gboolean
axis_meta_add_provider(
	AxisMeta                    *self,
	const AxisMetaProviderInfo  *info,
	GError                     **error
);

/**
 * axis_meta_add_reference:
 * @name: what refers to titles ("wanted", "library"), for messages
 * @sql: one SELECT statement (no ';') whose only column is the id of a
 *   meta_title that is referred to; a constant of the module's own code,
 *   never built from input
 *
 * Something that refers to titles, so that the refresh job keeps them
 * current (a wanted list, the library's identified files).  Before the
 * layer is used.  Returns: %FALSE (and a critical) for a statement that is
 * not of that form.
 */
gboolean
axis_meta_add_reference(
	AxisMeta    *self,
	const gchar *name,
	const gchar *sql
);

/**
 * axis_meta_get_setting_table:
 *
 * The module's settings: freshness, the artwork cap, and each provider's
 * key ("metadata.<name>_key", a secret) and rate ("metadata.<name>_rate").
 * The table lives as long as the process.
 *
 * Returns: (transfer none): settings ended by an entry whose key is %NULL
 */
const AxisSettingInfo *
axis_meta_get_setting_table(AxisMeta *self);

/**
 * AxisMetaTuning:
 * @request_timeout_ms: one request's deadline (0: the default)
 * @background_ms: how long a fetch nobody waits for may wait for its
 *   provider's budget (0: a minute)
 *
 * For tests.
 */
typedef struct
{
	guint request_timeout_ms;
	guint background_ms;
	guint token_renew_s;	/* a login's token is renewed after this (0: AXIS_META_TOKEN_RENEW_S) */
} AxisMetaTuning;

void
axis_meta_set_tuning(
	AxisMeta             *self,
	const AxisMetaTuning *tuning
);

/* The module's switch (its start and stop hooks). */
void
axis_meta_set_enabled(
	AxisMeta *self,
	gboolean  enabled
);

gboolean
axis_meta_is_enabled(AxisMeta *self);

/**
 * axis_meta_stop:
 *
 * Cancels what is being fetched and waits for the fetch threads.  Call it
 * after the job queue has stopped (its jobs use the layer); the layer
 * answers from the store only from then on.
 */
void
axis_meta_stop(AxisMeta *self);

AxisDbPool *
axis_meta_get_pool(AxisMeta *self);

/* ── Name keys ──────────────────────────────────────────────────── */

/**
 * axis_meta_name_keys:
 * @title: (nullable): a title's name
 * @original_title: (nullable): its original name
 * @other_names: (nullable) (array zero-terminated=1): its other names
 *
 * What meta_title.name_keys holds (0029_title_lists.sql): the comparison key
 * (axis_release_title_key()) of each name, once each, joined by '|'.  A list
 * filters by name by looking for the key of what was typed in it, so case,
 * accents, punctuation and roman numerals do not matter.
 *
 * Returns: (transfer full): the keys ("" when no name has one)
 */
gchar *
axis_meta_name_keys(
	const gchar        *title,
	const gchar        *original_title,
	const gchar *const *other_names
);

/**
 * axis_meta_fill_name_keys:
 * @limit: at most this many titles
 * @filled: (out) (optional): how many were filled
 *
 * Fills in name_keys for titles stored before it existed (every title
 * stored since gets them with it).  Cheap when there are none (an index of
 * the missing).  Blocks: a worker thread.
 *
 * Returns: %TRUE on success
 */
gboolean
axis_meta_fill_name_keys(
	AxisMeta  *self,
	guint      limit,
	guint     *filled,
	GError   **error
);

/**
 * axis_meta_fill_every_name_key:
 * @pool: the database, migrated
 *
 * Fills in every missing name_keys, in batches, until none is missing: the
 * daemon's start, after the migrations and before anything reads them, so
 * that a migration that empties keys made by an earlier comparison
 * (0052_identify_hunt_fixes.sql) leaves no time in which a page or the
 * view of episode files reads them empty.  Blocks.
 *
 * Returns: %TRUE on success
 */
gboolean
axis_meta_fill_every_name_key(
	AxisDbPool  *pool,
	GError     **error
);

/* ── Providers ──────────────────────────────────────────────────── */

guint
axis_meta_n_providers(AxisMeta *self);

const AxisMetaProviderInfo *
axis_meta_provider_at(
	AxisMeta *self,
	guint     index
);

/* Returns: (nullable): the provider called @name */
const AxisMetaProviderInfo *
axis_meta_provider_find(
	AxisMeta    *self,
	const gchar *name
);

/**
 * AxisMetaProviderStatus:
 * @info: the provider
 * @needs_key: it takes a key
 * @key_set: one is set (never what it is)
 * @limit: its budget and its state
 * @logs_in: its key is exchanged for a token (AxisMetaProviderInfo.login)
 * @key_refused: its login refused the key and PIN set now: nothing is sent
 *   to it until they change
 * @has_token: a token is held (never what it is)
 * @test_text: (nullable): what the latest "Test the key" said, safe to show
 * @test_ok: whether it worked
 * @test_us: when (real time), 0 if never
 */
typedef struct
{
	const AxisMetaProviderInfo *info;
	gboolean                    needs_key;
	gboolean                    key_set;
	AxisMetaLimitStatus         limit;
	gboolean                    logs_in;
	gboolean                    key_refused;
	gboolean                    has_token;
	gchar                      *test_text;
	gboolean                    test_ok;
	gint64                      test_us;
} AxisMetaProviderStatus;

void
axis_meta_provider_status(
	AxisMeta               *self,
	guint                   index,
	AxisMetaProviderStatus *status
);

void
axis_meta_provider_status_clear(AxisMetaProviderStatus *status);

/* Whether @info may be offered: it is not secondary, or its key is set
 * (a provider that needs none is always offered). */
gboolean
axis_meta_provider_offered(
	AxisMeta                   *self,
	const AxisMetaProviderInfo *info
);

/**
 * axis_meta_set_followed:
 * @sources: (array zero-terminated=1) (nullable): the sources of numbering
 *   that managed series follow now ("tmdb", "tvdb", ...)
 *
 * Says which providers' numbering managed series follow (the TV module
 * tells it, at its start and after each change of a series' numbering):
 * a provider is followed while one of @sources is its own.  A followed
 * provider's notice is shown on every page even without its key
 * (add-metadata-tvdb).  Any thread.
 */
void
axis_meta_set_followed(
	AxisMeta           *self,
	const gchar *const *sources
);

/* Whether a managed series follows one of @info's sources of numbering. */
gboolean
axis_meta_provider_followed(
	AxisMeta                   *self,
	const AxisMetaProviderInfo *info
);

/**
 * axis_meta_test_key:
 * @provider: a provider's name
 * @cancellable: (nullable): the caller's
 * @summary: (out) (transfer full): what came back, safe to show
 *
 * "Test the key": for a provider that offers a test (AxisMetaProviderInfo.test),
 * drops the token held, logs in anew and asks the provider's test question,
 * which is about no title and nothing of the library.  The outcome is kept
 * (AxisMetaProviderStatus.test_text) for the Metadata page.  A key refused
 * before is not sent again: the test says so.  Blocks: a worker thread.
 *
 * Returns: %TRUE when the provider answered; otherwise *@error says why
 *   (also in *@summary)
 */
gboolean
axis_meta_test_key(
	AxisMeta      *self,
	const gchar   *provider,
	GCancellable  *cancellable,
	gchar        **summary,
	GError       **error
);

/* ── Freshness ──────────────────────────────────────────────────── */

/* The freshness limits in effect, in seconds. */
typedef struct
{
	gint64 search;
	gint64 unreleased;
	gint64 released;
	gint64 ended;
	gint64 current_season;
	gint64 negative;
	gint64 viewed;
} AxisMetaFreshness;

void
axis_meta_get_freshness(
	AxisMeta          *self,
	AxisMetaFreshness *freshness
);

/**
 * axis_meta_title_period:
 * @title: a stored title
 * @freshness: the limits
 * @today: "YYYY-MM-DD"
 *
 * How long @title's record stays fresh: a released film (released, or of
 * unknown state with a release date before @today) as released; any other
 * film as unreleased; an ended series as ended; any other series as
 * unreleased (still running).
 *
 * Returns: seconds
 */
gint64
axis_meta_title_period(
	const AxisMetaTitle     *title,
	const AxisMetaFreshness *freshness,
	const gchar             *today
);

/* ── Reads ──────────────────────────────────────────────────────── */

/**
 * AxisMetaSearchAnswer:
 * @provider: who was asked (its attribution goes wherever this is shown)
 * @titles: (element-type AxisMetaTitle): the stored titles, in the
 *   provider's order (brief records: no seasons, no other names)
 * @fetched_us: when the answer shown was fetched; 0 if there is none
 * @stale: it is older than its freshness limit
 * @saved: the provider did not answer this time (in time): what is shown is
 *   what was saved before (or nothing, when @fetched_us is 0)
 * @problem: (nullable): why the provider did not answer, safe to show
 */
typedef struct
{
	const AxisMetaProviderInfo *provider;
	GPtrArray                  *titles;
	gint64                      fetched_us;
	gboolean                    stale;
	gboolean                    saved;
	gchar                      *problem;
} AxisMetaSearchAnswer;

void
axis_meta_search_answer_clear(AxisMetaSearchAnswer *answer);

/**
 * axis_meta_search:
 * @provider: a provider's name
 * @kind: what is looked for
 * @text: what was typed
 * @year: the year, or 0
 * @wait_ms: how long to wait for the provider when the store has no fresh
 *   answer (0: do not ask it; answer from the store)
 * @answer: (out): the answer; clear it with axis_meta_search_answer_clear()
 *
 * Fails only with %AXIS_META_ERROR_INVALID (no such provider, a kind it
 * does not serve, nothing to look for) or a database error.  A provider
 * that does not answer is not a failure (see @answer->saved).
 *
 * Returns: %TRUE when @answer is filled
 */
gboolean
axis_meta_search(
	AxisMeta              *self,
	const gchar           *provider,
	AxisMetaKind           kind,
	const gchar           *text,
	gint                   year,
	guint                  wait_ms,
	AxisMetaSearchAnswer  *answer,
	GError               **error
);

/* As axis_meta_search(), for page @page (1 or 2) of the provider's answer;
 * the second page is kept as a lookup of its own. */
gboolean
axis_meta_search_page(
	AxisMeta              *self,
	const gchar           *provider,
	AxisMetaKind           kind,
	const gchar           *text,
	gint                   year,
	guint                  page,
	guint                  wait_ms,
	AxisMetaSearchAnswer  *answer,
	GError               **error
);

/* As axis_meta_search(), for the titles @provider knows under @scheme:@value
 * (an IMDb id, say). */
gboolean
axis_meta_find(
	AxisMeta              *self,
	const gchar           *provider,
	AxisMetaKind           kind,
	const gchar           *scheme,
	const gchar           *value,
	guint                  wait_ms,
	AxisMetaSearchAnswer  *answer,
	GError               **error
);

/**
 * AxisMetaTitleAnswer:
 * @title: (nullable): the stored title with its ids, other names and
 *   seasons; %NULL when there is none
 * @not_found: the provider said it has no such title (an answer kept for
 *   metadata.fresh_negative)
 * @fetched_us, @stale, @saved, @problem: as in AxisMetaSearchAnswer, for
 *   the title's full record
 */
typedef struct
{
	const AxisMetaProviderInfo *provider;
	AxisMetaTitle              *title;
	gboolean                    not_found;
	gint64                      fetched_us;
	gboolean                    stale;
	gboolean                    saved;
	gchar                      *problem;
} AxisMetaTitleAnswer;

void
axis_meta_title_answer_clear(AxisMetaTitleAnswer *answer);

/**
 * axis_meta_lookup:
 * @id: the provider's own identifier of the title
 *
 * A title's full record, by the provider's id, as axis_meta_search() for
 * waiting and failing.  It counts as viewed.
 */
gboolean
axis_meta_lookup(
	AxisMeta             *self,
	const gchar          *provider,
	AxisMetaKind          kind,
	const gchar          *id,
	guint                 wait_ms,
	AxisMetaTitleAnswer  *answer,
	GError              **error
);

/* As axis_meta_lookup(), by the store's id (fails with
 * %AXIS_META_ERROR_NOT_FOUND when there is no such stored title). */
gboolean
axis_meta_get_title(
	AxisMeta             *self,
	gint64                title_id,
	guint                 wait_ms,
	AxisMetaTitleAnswer  *answer,
	GError              **error
);

typedef struct
{
	AxisMetaSeason *season;		/* (nullable) with its episodes */
	gint64          fetched_us;
	gboolean        stale;
	gboolean        saved;
	gboolean        not_found;
	gchar          *problem;
} AxisMetaSeasonAnswer;

void
axis_meta_season_answer_clear(AxisMetaSeasonAnswer *answer);

/**
 * axis_meta_get_source_season:
 * @source: (nullable): a source of numbering (axis_meta_source_valid():
 *   "tmdb", "tvdb", "tvdb-dvd"); %NULL for the record provider's own
 *
 * Season @number of @source of the stored series @title_id with its
 * episodes, as axis_meta_get_title() for waiting and failing.  Each source's
 * seasons are kept apart (0057_episode_sources.sql) and fetched from the
 * source's own provider, with that provider's identifier of the series (none
 * held: what is stored is all): a provider that numbers a series in several
 * orders (TheTVDB) is asked for the whole order, every page, stored in one
 * transaction; another for the one season.  No other source's season is
 * changed.
 */
gboolean
axis_meta_get_source_season(
	AxisMeta              *self,
	gint64                 title_id,
	const gchar           *source,
	gint                   number,
	guint                  wait_ms,
	AxisMetaSeasonAnswer  *answer,
	GError               **error
);

/**
 * axis_meta_load_source_seasons:
 * @source: (nullable): a source of numbering; %NULL for the one the title is
 *   followed in (its managed series' source, else its record provider's own)
 * @shown: (out) (transfer full): the source of the seasons given
 * @followed: (out) (optional) (transfer full): the source the title is
 *   followed in (its managed series', else its record provider's own)
 * @series_id: (out) (optional): its managed series, 0 for none
 * @seasons: (out) (transfer full) (element-type AxisMetaSeason): its seasons
 *   by number, without their episodes
 *
 * What the store holds of a series' seasons in one source; no provider is
 * asked.  Blocks: a worker thread.
 */
gboolean
axis_meta_load_source_seasons(
	AxisMeta     *self,
	gint64        title_id,
	const gchar  *source,
	gchar       **shown,
	gchar       **followed,
	gint64       *series_id,
	GPtrArray   **seasons,
	GError      **error
);

/* Season @number of the stored series @title_id with its episodes, as
 * axis_meta_get_title() for waiting and failing: the record provider's own
 * seasons (axis_meta_get_source_season() with no source named). */
gboolean
axis_meta_get_season(
	AxisMeta              *self,
	gint64                 title_id,
	gint                   number,
	guint                  wait_ms,
	AxisMetaSeasonAnswer  *answer,
	GError               **error
);

/**
 * axis_meta_load_titles:
 * @ids: (element-type gint64): stored titles' ids
 * @titles: (out) (transfer full) (element-type AxisMetaTitle): their brief
 *   records with their identifiers, in the order of @ids, those not stored
 *   left out
 *
 * What the store holds, nothing more: no provider is asked and no title
 * counts as viewed.  Blocks: a worker thread.
 *
 * Returns: %TRUE on success
 */
gboolean
axis_meta_load_titles(
	AxisMeta    *self,
	GArray      *ids,
	GPtrArray  **titles,
	GError     **error
);

/**
 * axis_meta_referenced_sql:
 *
 * One SELECT statement (no ';') whose only column, id, is every title a
 * module refers to (axis_meta_add_reference(): a wanted list, the library),
 * for use as a subquery.  Built from the modules' constant statements only.
 *
 * Returns: (transfer full): the statement
 */
gchar *
axis_meta_referenced_sql(AxisMeta *self);

/* ── Lists ──────────────────────────────────────────────────────── */

/* The lists @info offers for @kind (AXIS_META_LIST_BIT()), 0 for none. */
guint
axis_meta_provider_lists(
	const AxisMetaProviderInfo *info,
	AxisMetaKind                kind
);

/**
 * axis_meta_fetch_list:
 * @provider: a provider's name
 * @kind: films or series
 * @list: which list
 * @id: (nullable): for %AXIS_META_LIST_RECOMMENDED, the provider's own
 *   identifier of the title whose recommendations are asked for; %NULL
 *   otherwise
 * @page: 1 to %AXIS_META_LIST_MAX_PAGE
 * @cancellable: (nullable): the job's
 * @out: (out) (transfer full): the page; every entry's title is stored
 *   (a brief record) and carries the store's id
 *
 * Asks the provider here and now, on the caller's thread (a job's), within
 * its rate limit (waiting for its budget at most a minute), with its key, at
 * its own origin.  The answer is checked whole (axis_meta_list_page_check())
 * before anything of it is stored.  Fails with %AXIS_META_ERROR_INVALID when
 * there is no such provider, it does not offer @list for @kind, @page is out
 * of range or @id is missing or not one of its identifiers; otherwise as the
 * provider's functions fail (%AXIS_META_ERROR_NO_KEY, _UNAVAILABLE,
 * _RATE_LIMITED, _NOT_FOUND, _UNREADABLE, _CANCELLED).
 *
 * Returns: %TRUE on success
 */
gboolean
axis_meta_fetch_list(
	AxisMeta           *self,
	const gchar        *provider,
	AxisMetaKind        kind,
	AxisMetaList        list,
	const gchar        *id,
	guint               page,
	GCancellable       *cancellable,
	AxisMetaListPage  **out,
	GError            **error
);

/* As axis_meta_fetch_list(), for the provider's genres of @kind
 * (AxisMetaGenre, checked with axis_meta_genres_check()).  Nothing is
 * stored. */
gboolean
axis_meta_fetch_genres(
	AxisMeta      *self,
	const gchar   *provider,
	AxisMetaKind   kind,
	GCancellable  *cancellable,
	GPtrArray    **genres,
	GError       **error
);

/* ── Artwork ────────────────────────────────────────────────────── */

/* Whether @hash is a content hash as artwork is named: 64 lower-case
 * hexadecimal digits. */
gboolean
axis_meta_artwork_hash_valid(const gchar *hash);

/**
 * axis_meta_artwork_find:
 * @provider: the provider whose title carries @ref
 * @ref: (nullable): the title's poster or backdrop reference
 * @fetch: when it is not stored (or was refused long enough ago), queue it
 *   for fetching (in the background: this does not wait)
 *
 * Returns: (transfer full) (nullable): the content hash of the stored
 *   image, for /art/<hash>; %NULL when there is none yet
 */
gchar *
axis_meta_artwork_find(
	AxisMeta        *self,
	const gchar     *provider,
	const gchar     *ref,
	AxisMetaArtSize  size,
	gboolean         fetch
);

/**
 * axis_meta_artwork_find_shown:
 * @fetch: when it is neither stored nor refused, queue it for fetching (in
 *   the background), provided a stored title shows @ref
 * @refused: (out) (optional): set when the provider refused the image lately
 *   (it has no such image, or not one Axis keeps): asking again brings
 *   nothing until that refusal is older than metadata.fresh_negative
 *
 * axis_meta_artwork_find() for a page's placeholder that asks for its image
 * (/search/poster): the address comes from the request, so it is fetched
 * only when a stored title shows it, as its poster or its backdrop (what the
 * cleanup keeps); any other address is looked up and never fetched.
 *
 * Returns: (transfer full) (nullable): as axis_meta_artwork_find()
 */
gchar *
axis_meta_artwork_find_shown(
	AxisMeta        *self,
	const gchar     *provider,
	const gchar     *ref,
	AxisMetaArtSize  size,
	gboolean         fetch,
	gboolean        *refused
);

/**
 * axis_meta_artwork_find_many:
 * @providers: (array length=n): each title's provider
 * @refs: (array length=n): each title's reference (nullable)
 * @hashes: (out caller-allocates) (array length=n): each one's content hash
 *   (transfer full, nullable), as axis_meta_artwork_find() has it
 *
 * axis_meta_artwork_find() for a page of titles at once: one statement
 * reads every one's stored row, and one marks those found as used, whatever
 * @n.  Those not stored are queued for fetching as there (with @fetch).
 */
void
axis_meta_artwork_find_many(
	AxisMeta           *self,
	const gchar *const *providers,
	const gchar *const *refs,
	guint               n,
	AxisMetaArtSize     size,
	gboolean            fetch,
	gchar             **hashes
);

/**
 * axis_meta_artwork_fetch:
 * @hash: (out) (optional) (transfer full): the stored image's hash
 *
 * Fetches and keeps @ref at @size now, unless it is stored already.  An
 * image that is not kept fails with %AXIS_META_ERROR_REFUSED (it is
 * remembered as refused, with the reason, for metadata.fresh_negative).
 *
 * Returns: %TRUE when the image is stored
 */
gboolean
axis_meta_artwork_fetch(
	AxisMeta         *self,
	const gchar      *provider,
	const gchar      *ref,
	AxisMetaArtSize   size,
	gchar           **hash,
	GError          **error
);

/**
 * axis_meta_artwork_read:
 * @hash: a content hash (refused unless axis_meta_artwork_hash_valid())
 * @data: (out) (transfer full): the image as it was received
 * @content_type: (out) (transfer full): its stored type
 *
 * The file is opened by its hash beneath the artwork directory's
 * descriptor, never by a path made from anything else, and only if it is a
 * regular file of the stored size.  Fails with %AXIS_META_ERROR_NOT_FOUND.
 * It counts as used (for the cap).
 *
 * Returns: %TRUE on success
 */
gboolean
axis_meta_artwork_read(
	AxisMeta     *self,
	const gchar  *hash,
	GBytes      **data,
	gchar       **content_type,
	GError      **error
);

/* ── Jobs ───────────────────────────────────────────────────────── */

typedef struct
{
	guint titles;		/* titles fetched again */
	guint seasons;		/* seasons fetched again */
	guint failed;		/* fetches that failed */
	guint providers_skipped;	/* paused, backing off or without a key */
	guint searches_pruned;
} AxisMetaRefreshReport;

/**
 * axis_meta_run_refresh:
 * @context: (nullable): the job, for progress and cancellation
 * @report: (out) (optional): what was done
 *
 * One refresh run: for each provider, its stale referenced titles and
 * seasons, oldest first, at most %AXIS_META_REFRESH_BATCH of each, within
 * its rate limit; a provider that stops answering is left for the next
 * run.  Then saved searches older than %AXIS_META_SEARCH_KEEP_S are dropped.
 *
 * Returns: %FALSE only for a database error or cancellation
 */
gboolean
axis_meta_run_refresh(
	AxisMeta               *self,
	AxisJobContext         *context,
	AxisMetaRefreshReport  *report,
	GError                **error
);

typedef struct
{
	guint   unreferenced;	/* images no stored title refers to, dropped */
	guint   over_cap;	/* images dropped to keep under the cap */
	guint   files_removed;
	guint   missing;	/* rows whose file was gone, dropped */
	guint   strays_removed;	/* files nothing knew of, and leftovers */
	guint64 bytes_kept;
} AxisMetaCleanupReport;

gboolean
axis_meta_run_cleanup(
	AxisMeta               *self,
	AxisJobContext         *context,
	AxisMetaCleanupReport  *report,
	GError                **error
);

/* Queues a refresh now ("refresh now"), or finds the one queued. */
gboolean
axis_meta_request_refresh(
	AxisMeta  *self,
	gint64    *job_id,
	gboolean  *created,
	GError   **error
);

/* ── The page's figures ─────────────────────────────────────────── */

typedef struct
{
	gint64  films;
	gint64  series;
	gint64  ids;
	gint64  alt_titles;
	gint64  seasons;
	gint64  episodes;
	gint64  searches;
	gint64  artwork;
	gint64  artwork_bytes;
	gint64  artwork_refused;
	gint64  artwork_cap;
	gint64  store_bytes;		/* the tables and their indexes */
	gchar  *refresh_state;		/* the latest refresh job's state, or NULL */
	gint64  refresh_finished_us;
	gchar  *refresh_error;
} AxisMetaFigures;

void
axis_meta_figures_clear(AxisMetaFigures *figures);

gboolean
axis_meta_load_figures(
	AxisMeta         *self,
	AxisMetaFigures  *figures,
	GError          **error
);

/* (nullable) The data directory's path, for the page. */
const gchar *
axis_meta_get_data_dir_path(AxisMeta *self);

G_END_DECLS

#endif /* AXIS_META_H */
