/*
 * axis-indexer-model.h - what an indexer search is: a request, a release,
 * the states of a source, and the checks on what a source returns
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * GLib only, no I/O: the checks of outside input here (a fetch address, a
 * fetched file's kind, a request's fields) are fuzzed through
 * tests/fuzz/fuzz-indexer-prowlarr.c and tests/fuzz/fuzz-indexer-newznab.c.
 *
 * Everything an indexer says is hostile text: a release's name comes from
 * whoever uploaded it, an indexer's from whoever configured it.  Names are
 * cleaned (valid UTF-8, no control characters, space collapsed) and cut to a
 * length, and are shown only as text.  Nothing is parsed out of a name here:
 * that is the release parser's (release/axis-release.h), and this layer does
 * not call it.
 */

#ifndef AXIS_INDEXER_MODEL_H
#define AXIS_INDEXER_MODEL_H

#include <glib.h>

#include "core/axis-http-origin.h"
#include "download/axis-download-model.h"

G_BEGIN_DECLS

/* The most releases a search keeps; a longer list is cut. */
#define AXIS_INDEXER_MAX_RESULTS        1000

/* The most results read from an answer before it is given up on (accepted
 * or not), so that a list of nothing but junk ends. */
#define AXIS_INDEXER_MAX_SCAN           5000

/* A release's name and an indexer's, in characters. */
#define AXIS_INDEXER_NAME_MAX_CHARS     300
#define AXIS_INDEXER_INDEXER_MAX_CHARS  100

/* The most indexers a source's list keeps, and failed names a search keeps. */
#define AXIS_INDEXER_MAX_INDEXERS       500
#define AXIS_INDEXER_MAX_FAILED         50

/* Categories and flags of a release, and of a request. */
#define AXIS_INDEXER_MAX_CATEGORIES     16
#define AXIS_INDEXER_MAX_FLAGS          8
#define AXIS_INDEXER_FLAG_MAX_CHARS     32
#define AXIS_INDEXER_MAX_INDEXER_IDS    32

/* A request's title, in characters and in bytes (so that, written out in a
 * path with every byte as %XX, it leaves room for the rest of it). */
#define AXIS_INDEXER_TITLE_MAX_CHARS    200
#define AXIS_INDEXER_TITLE_MAX_BYTES    256

/* The longest fetch reference (path and query) kept, in bytes.  A real
 * Prowlarr's link for a torrent wraps the whole magnet link and runs to
 * about 2,350 bytes. */
#define AXIS_INDEXER_FETCH_PATH_MAX     AXIS_HTTP_MAX_PATH_BYTES

/* The largest file fetched: the largest the downloads layer uploads. */
#define AXIS_INDEXER_MAX_FILE_BYTES     AXIS_DOWNLOAD_MAX_RELEASE_BYTES

/* ── Source kinds' words ────────────────────────────────────────── */

/* 1 to 16 lower-case letters and digits, starting with a letter. */
gboolean
axis_indexer_kind_name_valid(const gchar *text);

/**
 * AxisIndexerSourceState:
 * @AXIS_INDEXER_SOURCE_UNCHECKED: not contacted since it was configured
 * @AXIS_INDEXER_SOURCE_OK: the last contact worked
 * @AXIS_INDEXER_SOURCE_UNREACHABLE: it could not be reached or did not
 *   answer in time
 * @AXIS_INDEXER_SOURCE_REFUSED: it refused the key
 * @AXIS_INDEXER_SOURCE_FAILING: it answered, but not usefully
 */
typedef enum
{
	AXIS_INDEXER_SOURCE_UNCHECKED,
	AXIS_INDEXER_SOURCE_OK,
	AXIS_INDEXER_SOURCE_UNREACHABLE,
	AXIS_INDEXER_SOURCE_REFUSED,
	AXIS_INDEXER_SOURCE_FAILING
} AxisIndexerSourceState;

const gchar *
axis_indexer_source_state_to_string(AxisIndexerSourceState state);

gboolean
axis_indexer_source_state_from_string(
	const gchar            *text,
	AxisIndexerSourceState *state
);

/* "Not checked yet", "Working", "Not answering", "Key refused", "Failing" */
const gchar *
axis_indexer_source_state_label(AxisIndexerSourceState state);

/* ── Requests ───────────────────────────────────────────────────── */

/**
 * AxisIndexerQueryKind:
 * @AXIS_INDEXER_QUERY_FILM: a film: its identifiers, else its title and year
 * @AXIS_INDEXER_QUERY_EPISODE: one episode of a series
 * @AXIS_INDEXER_QUERY_SEASON: a whole season (releases of it, and packs)
 * @AXIS_INDEXER_QUERY_TEXT: words, as typed
 */
typedef enum
{
	AXIS_INDEXER_QUERY_FILM,
	AXIS_INDEXER_QUERY_EPISODE,
	AXIS_INDEXER_QUERY_SEASON,
	AXIS_INDEXER_QUERY_TEXT
} AxisIndexerQueryKind;

const gchar *
axis_indexer_query_kind_to_string(AxisIndexerQueryKind kind);

gboolean
axis_indexer_query_kind_from_string(
	const gchar          *text,
	AxisIndexerQueryKind *kind
);

/**
 * AxisIndexerRequest:
 * @kind: what is looked for
 * @title: (nullable): the title (a film's, a series'), or the words of a
 *   text search; borrowed
 * @year: a film's year, 0 for none
 * @imdb_id: (nullable): "tt" and 7 to 10 digits; borrowed
 * @tmdb_id: the TMDb identifier, 0 for none
 * @tvdb_id: the TVDB identifier, 0 for none
 * @season: a season's number (0 is specials), -1 for none
 * @episode: an episode's number (1 or more), -1 for none
 * @n_categories: how many of @categories are set; none means the kind's
 *   own (films 2000, series 5000)
 * @categories: Newznab categories to search in
 * @n_indexers: how many of @indexers are set; none means every indexer of
 *   the source
 * @indexers: the source's own identifiers of the indexers to ask
 * @automatic: the search is made by a schedule or a module, not asked for by
 *   a person: an indexer near its daily allowance is left out, keeping what
 *   is left for searches a person asks for.  Not part of the request's key
 * @anime: the search is for a series marked as anime (add-anime-support, A3):
 *   when the request names no categories, an episode, a season and words are
 *   asked in the TV categories and in TV/Anime (%AXIS_INDEXER_CATEGORY_ANIME)
 *   as well.  %FALSE: the request is exactly what it is without this field,
 *   and has the key it has without it
 *
 * What to find, as the modules know it.  A source uses the identifiers where
 * its indexers accept them and the words otherwise; the result says which.
 * Initialise with axis_indexer_request_init().
 */
typedef struct
{
	AxisIndexerQueryKind  kind;
	const gchar          *title;
	gint                  year;
	const gchar          *imdb_id;
	gint64                tmdb_id;
	gint64                tvdb_id;
	gint                  season;
	gint                  episode;
	guint                 n_categories;
	gint                  categories[AXIS_INDEXER_MAX_CATEGORIES];
	guint                 n_indexers;
	gint64                indexers[AXIS_INDEXER_MAX_INDEXER_IDS];
	gboolean              automatic;
	gboolean              anime;
} AxisIndexerRequest;

/* Newznab's and Torznab's category for anime, a subcategory of TV (5000). */
#define AXIS_INDEXER_CATEGORY_ANIME 5070

/**
 * axis_indexer_request_tv_categories:
 * @own: (array length=n_own) (nullable): the TV categories an indexer is
 *   searched in; none means 5000
 * @out: (out caller-allocates): %AXIS_INDEXER_MAX_CATEGORIES numbers
 *
 * The categories a request that names none is asked in for TV, when it is for
 * an anime series: @own (or 5000), and 5070 after them unless it is among
 * them already or there is no room for it.  Returns how many.
 */
guint
axis_indexer_request_tv_categories(
	const gint *own,
	guint       n_own,
	gint       *out
);

void
axis_indexer_request_init(
	AxisIndexerRequest   *request,
	AxisIndexerQueryKind  kind
);

/* Whether @imdb_id is "tt" and 7 to 10 digits. */
gboolean
axis_indexer_imdb_valid(const gchar *text);

/**
 * axis_indexer_request_check:
 * @problem: (out) (optional) (transfer none): why not, a fixed sentence
 *   fragment that holds none of the request
 *
 * Whether the request can be made: its fields are in range and it names
 * something to look for (a text search needs words; a film, an identifier
 * or a title; an episode, a season and an episode number and a series
 * identifier or title; a season, a season number and the same).
 */
gboolean
axis_indexer_request_check(
	const AxisIndexerRequest *request,
	const gchar             **problem
);

/**
 * axis_indexer_request_key:
 *
 * The normalised request, as a SHA-256 in hex (64 characters): the same
 * request however its title is cased or spaced, its categories or indexers
 * ordered.  The words that were searched for are not kept.
 */
gchar *
axis_indexer_request_key(const AxisIndexerRequest *request);

/**
 * AxisIndexerMode:
 * @AXIS_INDEXER_BY_TEXT: the title and year (or the words)
 * @AXIS_INDEXER_BY_IMDB: an IMDb identifier
 * @AXIS_INDEXER_BY_TMDB: a TMDb identifier
 * @AXIS_INDEXER_BY_TVDB: a TVDB identifier
 *
 * How a search was made.
 */
typedef enum
{
	AXIS_INDEXER_BY_TEXT,
	AXIS_INDEXER_BY_IMDB,
	AXIS_INDEXER_BY_TMDB,
	AXIS_INDEXER_BY_TVDB
} AxisIndexerMode;

const gchar *
axis_indexer_mode_to_string(AxisIndexerMode mode);

gboolean
axis_indexer_mode_from_string(
	const gchar     *text,
	AxisIndexerMode *mode
);

/* "title and year", "IMDb identifier", ... */
const gchar *
axis_indexer_mode_label(AxisIndexerMode mode);

/* ── Releases ───────────────────────────────────────────────────── */

/**
 * AxisIndexerRelease:
 * @id: the stored release's identifier (0 before it is stored); a grab
 *   refers to a release by this and not by an address
 * @name: the release's name as the indexer gave it, cleaned and cut
 * @size_bytes: -1 when not stated
 * @protocol: usenet or torrent
 * @indexer_id: the source's own identifier of the indexer, 0 when not stated
 * @indexer_name: (nullable): the indexer, cleaned and cut
 * @published_us: when it was published, microseconds since 1970; 0 when not
 *   stated
 * @seeders, @leechers, @grabs: -1 when not stated (a Usenet release has
 *   neither seeders nor leechers)
 * @n_categories, @categories: Newznab category numbers, as stated
 * @flags: (nullable): the indexer's flags ("freeleech"), cleaned; a
 *   %NULL-ended vector or %NULL for none
 * @imdb_id: (nullable): "tt..." as the indexer states it
 * @tmdb_id, @tvdb_id: as stated, 0 for none
 * @info_hash: (nullable): a torrent's info-hash as stated, 40 lower-case hex
 *   digits
 * @has_magnet: a magnet link is stored for it (never shown)
 * @source_id: the source (or native indexer) that returned it
 * @n_alternatives: how many other indexers returned the same release in the
 *   same search (by info-hash for a torrent, by exact name and size for
 *   Usenet); a fetch that fails here is tried there before it fails
 * @also_at: (nullable): those indexers' names, a %NULL-ended vector
 *
 * A release as a search returns it.  It holds no fetch reference and no
 * magnet link: those are secrets, read only by axis_indexers_fetch().
 */
typedef struct
{
	gint64                id;
	gchar                *name;
	gint64                size_bytes;
	AxisDownloadProtocol  protocol;
	gint64                indexer_id;
	gchar                *indexer_name;
	gint64                published_us;
	gint                  seeders;
	gint                  leechers;
	gint                  grabs;
	guint                 n_categories;
	gint                  categories[AXIS_INDEXER_MAX_CATEGORIES];
	gchar               **flags;
	gchar                *imdb_id;
	gint64                tmdb_id;
	gint64                tvdb_id;
	gchar                *info_hash;
	gboolean              has_magnet;
	gint64                source_id;
	guint                 n_alternatives;
	gchar               **also_at;
} AxisIndexerRelease;

AxisIndexerRelease *
axis_indexer_release_new(void);

void
axis_indexer_release_free(AxisIndexerRelease *release);

/* A deep copy. */
AxisIndexerRelease *
axis_indexer_release_copy(const AxisIndexerRelease *release);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisIndexerRelease, axis_indexer_release_free)

/**
 * axis_indexer_release_group_key:
 *
 * What makes two releases from different indexers the same one: a torrent
 * by its info-hash, a Usenet release by its exact name and its size (both
 * stated).  A SHA-256 in hex; %NULL when the release states too little to be
 * matched (a torrent without a hash, a Usenet release without a size).
 */
gchar *
axis_indexer_release_group_key(const AxisIndexerRelease *release);

/**
 * AxisIndexerFound:
 * @release: the release, with no identifier yet
 * @fetch_path: (nullable): the path and query on the source's own origin
 *   where the release is fetched, checked to be under the source's base
 *   path; a SECRET (it holds the source's key): never log it, never show it
 * @magnet: (nullable): a checked magnet link; a SECRET too (a tracker's
 *   passkey can be in it)
 * @merge_key: (nullable): what tells this release apart from the others of
 *   a source, for merging two answers: a SHA-256 in hex of the source's own
 *   identifier of the result (Prowlarr's "guid", which can be an address
 *   with a passkey or a whole magnet link, so it is never kept as it is);
 *   %NULL when the result had none.  Never stored
 * @files_origin: @fetch_path is on the native indexer's other origin, the
 *   one an administrator accepted for its files, and not on its own
 *
 * What a kind's search hands the layer for each release it accepted.  At
 * least one of @fetch_path and @magnet is set.
 */
typedef struct
{
	AxisIndexerRelease *release;
	gchar              *fetch_path;
	gchar              *magnet;
	gchar              *merge_key;
	gboolean            files_origin;
} AxisIndexerFound;

void
axis_indexer_found_free(AxisIndexerFound *found);

/**
 * AxisIndexerOutcome:
 * @items: (element-type AxisIndexerFound): the releases accepted, at most
 *   %AXIS_INDEXER_MAX_RESULTS
 * @n_off_origin: results left out because they named a fetch address that is
 *   not the source's own
 * @n_unusable: results left out because there was no way to fetch them
 * @n_skipped: results left out because they could not be read
 * @truncated: there were more results than were kept
 * @mode: how the search was made
 * @fell_back: an identifier search found nothing and the title was searched
 *   instead (@mode is then text)
 * @also_text: besides the search by the identifier (@mode), the indexers
 *   that take no identifier were asked by the title, and the two answers
 *   merged (the identifier's first, each release once)
 * @other_origin: (nullable): the first origin ("https://host:port") a result
 *   named for its file that is neither the indexer's own nor the one
 *   accepted for it: shown to an administrator at a test, to accept or not;
 *   never used to fetch anything
 * @not_asked: (nullable): set (by axis_indexer_outcome_nothing_here()) when
 *   the kind has nothing for this kind of search (a TV site for a movie) and
 *   returned without making a request: why, in words that follow "Not asked:"
 *   ("it has no movies").  Such an outcome holds no releases, was never a
 *   request to the indexer, and is not stored or counted as an answer
 * @n_listed: how many results the answer held, kept or not (Newznab and
 *   Torznab only: the step to the next page)
 * @total: how many results the indexer says it has for the search, or -1
 *   when it does not say (Newznab and Torznab only; read from a page)
 *
 * What a kind's search returns.
 */
typedef struct
{
	GPtrArray       *items;
	guint            n_off_origin;
	guint            n_unusable;
	guint            n_skipped;
	guint            n_listed;
	gint64           total;
	gboolean         truncated;
	AxisIndexerMode  mode;
	gboolean         fell_back;
	gboolean         also_text;
	gchar           *other_origin;
	gchar           *not_asked;
} AxisIndexerOutcome;

void
axis_indexer_outcome_init(AxisIndexerOutcome *outcome);

/*
 * Marks @outcome as "nothing here for this kind of search, and no request
 * was made": for a movie search ("it has no movies") or a TV one ("it has no
 * TV"), by @request's kind.  A kind calls it before it returns %TRUE without
 * having asked the indexer anything.
 */
void
axis_indexer_outcome_nothing_here(
	AxisIndexerOutcome       *outcome,
	const AxisIndexerRequest *request
);

void
axis_indexer_outcome_clear(AxisIndexerOutcome *outcome);

/**
 * AxisIndexerInfo:
 * @id: the source's own identifier of the indexer
 * @name: cleaned and cut
 * @protocol: usenet or torrent
 * @enabled: switched on at the source
 * @failing: the source says it is failing now (disabled after failures)
 * @movie_ids, @tv_ids: the identifiers it takes for a movie and for an
 *   episode, bits %AXIS_INDEXER_IDS_IMDB and so on
 * @redirect: the source answers its downloads with a redirect to the
 *   indexer's own site (Prowlarr's per-indexer "Redirect" option) instead of
 *   the file
 * @host: (nullable): the host the indexer is at, as axis_indexer_host_key()
 *   gives it, when the source says (Prowlarr's indexerUrls): an indexer
 *   also configured natively at that host is asked natively and not here
 *
 * One indexer a source reports.  A native indexer (Newznab, Torznab) reports
 * itself, with the identifiers its capabilities name.
 */
typedef struct
{
	gint64                id;
	gchar                *name;
	AxisDownloadProtocol  protocol;
	gboolean              enabled;
	gboolean              failing;
	guint                 movie_ids;
	guint                 tv_ids;
	gboolean              redirect;
	gchar                *host;
} AxisIndexerInfo;

#define AXIS_INDEXER_IDS_IMDB 1
#define AXIS_INDEXER_IDS_TMDB 2
#define AXIS_INDEXER_IDS_TVDB 4

AxisIndexerInfo *
axis_indexer_info_new(void);

void
axis_indexer_info_free(AxisIndexerInfo *info);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisIndexerInfo, axis_indexer_info_free)

/* "IMDb, TMDb" for @ids, "none" for 0. */
gchar *
axis_indexer_ids_label(guint ids);

/**
 * axis_indexer_host_key:
 * @host: a host name or address
 *
 * The host in lower case, without a leading "www." or "api.": what two
 * addresses of one indexer share ("api.newsowl.example" and "newsowl.example").
 * %NULL for a host that is empty or not plain.
 */
gchar *
axis_indexer_host_key(const gchar *host);

/* The host of an address ("https://api.newsowl.example/"), as
 * axis_indexer_host_key() gives it; %NULL when it has none. */
gchar *
axis_indexer_url_host_key(const gchar *url);

/* @text's letters and digits in lower case: how a name is compared with
 * another ("News-Owl" and "newsowl" are the same). */
gchar *
axis_indexer_plain_name(const gchar *text);

/* The name a site goes by, from a host key: the label before the last dot,
 * plain ("newsowl" for "newsowl.example" and for "api.newsowl.example" alike).
 * %NULL for an address (it has no name), a host of one label, and a label of
 * under three characters (the "co" of "example.co.uk"). */
gchar *
axis_indexer_site_name(const gchar *host);

/* ── A native indexer's capabilities and allowances ─────────────── */

#define AXIS_INDEXER_CAN_SEARCH 1
#define AXIS_INDEXER_CAN_MOVIE  2
#define AXIS_INDEXER_CAN_TV     4

/* The most categories of an indexer's capabilities kept. */
#define AXIS_INDEXER_MAX_CAPS_CATEGORIES 100

/**
 * AxisIndexerCaps:
 * @read_us: when they were last read (0: never)
 * @problem: (nullable): why the last reading failed (fixed text)
 * @modes: the searches it has, %AXIS_INDEXER_CAN_SEARCH and so on
 * @movie_ids, @tv_ids: the identifiers its movie and its TV search take
 * @movie_text, @tv_text: whether those take words (q)
 * @tv_numbers: whether its TV search takes a season and an episode
 * @limit_max, @limit_default: the most results a search answers, and how
 *   many when not told; 0 when not stated
 * @categories: (nullable): its categories, "2000 Movies" and so on, cleaned
 *   (at most %AXIS_INDEXER_MAX_CAPS_CATEGORIES)
 *
 * What a Newznab or Torznab indexer's t=caps said it can do.
 */
typedef struct
{
	gint64    read_us;
	gchar    *problem;
	guint     modes;
	guint     movie_ids;
	guint     tv_ids;
	gboolean  movie_text;
	gboolean  tv_text;
	gboolean  tv_numbers;
	gint      limit_max;
	gint      limit_default;
	gchar   **categories;
} AxisIndexerCaps;

void
axis_indexer_caps_clear(AxisIndexerCaps *caps);

void
axis_indexer_caps_copy(
	const AxisIndexerCaps *from,
	AxisIndexerCaps       *to
);

/**
 * AxisIndexerAllowance:
 * @api_used, @api_max: API calls made in the last day and allowed in one;
 *   -1 when not known
 * @grab_used, @grab_max: the same for files fetched
 * @reported_us: when the indexer last said (0: never)
 *
 * A native indexer's daily allowances, as it reports them
 * (<newznab:apilimits>), counted on by Axis between reports.
 */
typedef struct
{
	gint    api_used;
	gint    api_max;
	gint    grab_used;
	gint    grab_max;
	gint64  reported_us;
} AxisIndexerAllowance;

void
axis_indexer_allowance_init(AxisIndexerAllowance *allowance);

/* What automatic searches leave of an allowance of @max: a tenth, and at
 * least five. */
gint
axis_indexer_allowance_reserve(gint max);

/* Whether an automatic search stops short of the indexer (what is left is
 * within the reserve), or, with @automatic %FALSE, whether nothing at all
 * is left. */
gboolean
axis_indexer_allowance_spent(
	const AxisIndexerAllowance *allowance,
	gboolean                    automatic
);

/* Whether no file may be fetched (the grabs are used up). */
gboolean
axis_indexer_allowance_grabs_spent(const AxisIndexerAllowance *allowance);

/* ── What a source returns, checked ─────────────────────────────── */

/* A release name or an indexer's, cleaned and cut; %NULL when nothing is
 * left. */
gchar *
axis_indexer_text_clean(
	const gchar *text,
	guint        max_chars
);

/**
 * axis_indexer_path_ok:
 * @path: a path with an optional query, as it is asked of the source
 * @base_path: the base path the source is served under ("" for none)
 *
 * Whether @path is something to ask the source's own origin for: it begins
 * with a slash, is at most %AXIS_INDEXER_FETCH_PATH_MAX bytes of printable
 * ASCII without space, backslash or '#', is under @base_path (@base_path
 * itself or @base_path and a '/'), and leaves it by no "." or ".." part and
 * no encoded dot, slash or backslash in its path.
 */
gboolean
axis_indexer_path_ok(
	const gchar *path,
	const gchar *base_path
);

typedef enum
{
	AXIS_INDEXER_REF_OK,
	AXIS_INDEXER_REF_OFF_ORIGIN,	/* an address, but not the source's own */
	AXIS_INDEXER_REF_INVALID	/* not an address that could be used */
} AxisIndexerRefResult;

/**
 * axis_indexer_fetch_ref:
 * @url: what the source gave as the address of a release's file
 * @origin: the source's configured origin
 * @base_path: its base path ("" for none)
 * @path: (out) (transfer full): for %AXIS_INDEXER_REF_OK, the path and query
 *   to ask of the origin (a secret)
 *
 * A fetch reference is accepted only if it is an http or https address with
 * exactly the source's scheme, host and port, no credentials and no
 * fragment, and a path that axis_indexer_path_ok() accepts under the
 * source's base path.  Whatever else it is, nothing is ever requested from
 * it.
 */
AxisIndexerRefResult
axis_indexer_fetch_ref(
	const gchar          *url,
	const AxisHttpOrigin *origin,
	const gchar          *base_path,
	gchar               **path
);

/**
 * AxisIndexerContent:
 * @AXIS_INDEXER_CONTENT_NONE: neither
 * @AXIS_INDEXER_CONTENT_NZB: an XML document whose root is <nzb>
 * @AXIS_INDEXER_CONTENT_TORRENT: a bencoded torrent
 */
typedef enum
{
	AXIS_INDEXER_CONTENT_NONE,
	AXIS_INDEXER_CONTENT_NZB,
	AXIS_INDEXER_CONTENT_TORRENT
} AxisIndexerContent;

/**
 * axis_indexer_content_kind:
 * @data: what was fetched
 *
 * What a fetched file is, by its bytes and nothing else (the content type a
 * server states is not believed).  An NZB begins, after an optional byte
 * order mark, space, an XML declaration, comments and a doctype without an
 * internal subset, with an "<nzb" element: the XML is not parsed.  A torrent
 * is a whole bencoded file with an "info" dictionary
 * (download/axis-download-bencode.h).
 */
AxisIndexerContent
axis_indexer_content_kind(
	const guint8 *data,
	gsize         length
);

/**
 * AxisIndexerFetchRequest:
 * @fetch_path: (nullable): the release's fetch reference (a secret)
 * @magnet: (nullable): its magnet link (a secret)
 * @protocol: what the release is
 * @name: its name
 * @info_hash: (nullable): the info-hash the search stated
 * @files_origin: @fetch_path is on the native indexer's accepted other
 *   origin for files, not its own
 *
 * What the layer hands a kind's fetch.
 */
typedef struct
{
	const gchar          *fetch_path;
	const gchar          *magnet;
	AxisDownloadProtocol  protocol;
	const gchar          *name;
	const gchar          *info_hash;
	gboolean              files_origin;
} AxisIndexerFetchRequest;

/**
 * AxisIndexerFetched:
 * @protocol: what it is
 * @file: (nullable): the .nzb or .torrent as it was fetched, unchanged
 * @magnet: (nullable): a checked magnet link, instead of a file
 * @file_name: (nullable): "<name>.nzb" or "<name>.torrent", safe as a name
 * @info_hash: (nullable): a torrent's info-hash (read from the file, or from
 *   the magnet link)
 * @source_id, @release_id: where it came from: the release asked for, or
 *   another indexer's copy of it when that one could not be fetched
 *
 * What a fetch returns: exactly one of @file and @magnet, in the words of
 * download/axis-download.h's request.
 */
typedef struct
{
	AxisDownloadProtocol  protocol;
	GBytes               *file;
	gchar                *magnet;
	gchar                *file_name;
	gchar                *info_hash;
	gint64                source_id;
	gint64                release_id;
} AxisIndexerFetched;

void
axis_indexer_fetched_clear(AxisIndexerFetched *fetched);

/* A file name for @name and @extension ("nzb"): letters, digits and a few
 * marks, at most 120 bytes, never empty. */
gchar *
axis_indexer_file_name(
	const gchar *name,
	const gchar *extension
);

G_END_DECLS

#endif /* AXIS_INDEXER_MODEL_H */
