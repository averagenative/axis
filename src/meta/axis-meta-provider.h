/*
 * axis-meta-provider.h - what a metadata provider is: a table of functions
 * and a few facts about it
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A provider (TMDb, later TVDB and others) is a static AxisMetaProviderInfo
 * in a file of its own, which turns the provider's answers into the generic
 * model (meta/axis-meta-model.h).  It never makes a request itself: it asks
 * the metadata layer with axis_meta_fetch_get(), which sends it to the
 * provider's own API origin (declared here, never taken from an answer),
 * with the user's key in the header the provider names, the fixed user
 * agent, and nothing else, within the provider's rate limit.  A provider's
 * paths carry only the search text or a title's identifier (escape them
 * with g_uri_escape_string()).
 *
 * Artwork is fetched by the layer itself from the image origin declared
 * here, at a path image_path() makes from the reference a title carries and
 * a size; the key is never sent there.
 *
 * Every function runs on a worker thread and may block (on the limiter and
 * the network).  An answer that cannot be read fails with
 * %AXIS_META_ERROR_UNREADABLE; one that says there is no such title with
 * %AXIS_META_ERROR_NOT_FOUND; anything axis_meta_fetch_get() reported is
 * passed on as it is.
 */

#ifndef AXIS_META_PROVIDER_H
#define AXIS_META_PROVIDER_H

#include <glib.h>

#include "core/axis-setting.h"
#include "meta/axis-meta-model.h"

G_BEGIN_DECLS

#define AXIS_META_ERROR (axis_meta_error_quark())

/**
 * AxisMetaError:
 * @AXIS_META_ERROR_NOT_FOUND: the provider has no such title (a negative
 *   answer, kept for a day)
 * @AXIS_META_ERROR_UNREADABLE: the answer could not be read; nothing of it
 *   is kept
 * @AXIS_META_ERROR_UNAVAILABLE: the provider could not be asked or did not
 *   answer (unreachable, too slow, an error status, the key refused)
 * @AXIS_META_ERROR_RATE_LIMITED: the provider asked to be left alone for a
 *   while, or its budget allows no request in time
 * @AXIS_META_ERROR_NO_KEY: the provider needs a key and none is set
 * @AXIS_META_ERROR_INVALID: the question is not one that can be asked (no
 *   such provider, a kind it does not serve, empty text)
 * @AXIS_META_ERROR_REFUSED: an image was not one Axis keeps
 * @AXIS_META_ERROR_CANCELLED: the daemon or the job is stopping
 * @AXIS_META_ERROR_FAILED: anything else (the data directory, the store)
 */
typedef enum
{
	AXIS_META_ERROR_NOT_FOUND,
	AXIS_META_ERROR_UNREADABLE,
	AXIS_META_ERROR_UNAVAILABLE,
	AXIS_META_ERROR_RATE_LIMITED,
	AXIS_META_ERROR_NO_KEY,
	AXIS_META_ERROR_INVALID,
	AXIS_META_ERROR_REFUSED,
	AXIS_META_ERROR_CANCELLED,
	AXIS_META_ERROR_FAILED
} AxisMetaError;

GQuark
axis_meta_error_quark(void);

/**
 * AxisMetaArtSize:
 * @AXIS_META_ART_POSTER_SMALL: a poster for cards (about 185 pixels wide)
 * @AXIS_META_ART_POSTER: a larger poster for a title's page (about 500)
 * @AXIS_META_ART_BACKDROP: a backdrop for a title's page (about 1280)
 *
 * The only sizes ever asked for.
 */
typedef enum
{
	AXIS_META_ART_POSTER_SMALL,
	AXIS_META_ART_POSTER,
	AXIS_META_ART_BACKDROP
} AxisMetaArtSize;

#define AXIS_META_ART_N_SIZES 3

/* "poster-small", "poster", "backdrop" */
const gchar *
axis_meta_art_size_to_string(AxisMetaArtSize size);

/* The layer's handle on one provider's requests (see axis-meta.h). */
typedef struct _AxisMetaFetch AxisMetaFetch;

/**
 * axis_meta_fetch_get:
 * @fetch: the handle the provider function was given
 * @path: an absolute path with an optional query, on the provider's API
 *   origin; never a full URL
 * @body: (out) (transfer full): the answer
 * @error: (nullable): return location for an %AXIS_META_ERROR
 *
 * One GET to the provider's API: waits for the provider's budget (as long
 * as the caller may wait), sends the key in the provider's header, and
 * fails with %AXIS_META_ERROR_NOT_FOUND for a 404, %AXIS_META_ERROR_RATE_LIMITED
 * for a 429 (after pausing the provider for the time it states),
 * %AXIS_META_ERROR_NO_KEY without a key, and %AXIS_META_ERROR_UNAVAILABLE for
 * anything else.  No message holds the key.
 *
 * Returns: %TRUE on a 2xx answer
 */
gboolean
axis_meta_fetch_get(
	AxisMetaFetch  *fetch,
	const gchar    *path,
	GBytes        **body,
	GError        **error
);

/**
 * axis_meta_fetch_get_setting:
 * @fetch: the handle the provider function was given
 * @key: a setting the provider declared (AxisMetaProviderInfo.settings)
 *
 * The current value of one of the provider's own string, choice or boolean
 * settings, as text ("true" or "false" for a boolean), so that a change on
 * the Settings page applies to the next request.
 *
 * Returns: (transfer full) (nullable): the value, or %NULL when there is no
 *   settings store or no such setting
 */
gchar *
axis_meta_fetch_get_setting(
	AxisMetaFetch *fetch,
	const gchar   *key
);

/**
 * axis_meta_fetch_login:
 * @fetch: the handle the provider's login function was given
 * @body: (out) (transfer full): the answer (the token in it, for the
 *   provider to read)
 *
 * For a provider that logs in (AxisMetaProviderInfo.login), and only from
 * its login function: the one POST of the provider's key, and its PIN when
 * one is set, in the body of the pinned login request at the provider's API
 * origin (core/axis-http.h), within its budget.  The
 * layer reads the key and the PIN from the settings itself: a provider never
 * sees them.  A login answered 401 or 403 marks the key refused: nothing is
 * sent to the provider again until the key or the PIN changes.  Fails as
 * axis_meta_fetch_get() does, and with %AXIS_META_ERROR_INVALID outside a
 * login.
 *
 * Returns: %TRUE on a 2xx answer
 */
gboolean
axis_meta_fetch_login(
	AxisMetaFetch  *fetch,
	GBytes        **body,
	GError        **error
);

typedef struct _AxisMetaProviderInfo AxisMetaProviderInfo;

/**
 * AxisMetaProviderInfo:
 * @name: 1 to 16 lower-case letters and digits; also the scheme of its own
 *   identifiers, and part of its settings' keys
 * @label: its name for people ("TMDb")
 * @kinds: the kinds it serves (AXIS_META_KIND_BIT())
 * @attribution: the notice its terms require wherever its data is shown
 * @api_origin: where its API is ("https://api.themoviedb.org"); requests
 *   go there and nowhere else
 * @image_origin: where its images are; artwork is fetched from there and
 *   nowhere else
 * @default_rate: its stated request rate, a second (conservative); the
 *   setting "metadata.<name>_rate" starts at it
 * @burst: how many requests may start at once
 * @key_header: (nullable): the request header its key goes in
 *   ("Authorization"); %NULL for a provider that needs no key
 * @key_prefix: (nullable): put before the key in that header ("Bearer ")
 * @key_help: (nullable): the Settings page's words for its key (where to
 *   get one)
 * @search: titles of @kind whose name matches @text (and of @year, when not
 *   0), at most %AXIS_META_MAX_RESULTS, as brief records (detail %FALSE):
 *   page @page (1 or 2) of the provider's answer
 * @title: the full record of the provider's title @id of @kind
 * @season: season @number of the series @series_id, with its episodes
 * @find_by_external: the titles of @kind the provider knows under another
 *   scheme's identifier ("imdb", "tt0500004")
 * @image_path: (transfer full) (nullable): the path on @image_origin of the
 *   image @ref at @size, or %NULL when there is none
 * @settings: (nullable): the provider's own settings besides its key and
 *   rate (language, region and the like), @n_settings of them, keys
 *   "metadata.<name>_<setting>"; they join the layer's table and are read
 *   with axis_meta_fetch_get_setting()
 * @n_settings: how many
 * @prepare_settings: (nullable): called when the layer hands out its
 *   settings table, after its own checks are registered: the place for the
 *   provider to register the checks of its settings
 *   (axis_setting_set_validator()), including a stricter one for its key
 * @data: the provider's own, for its functions
 * @list: (nullable): page @page (1 and up) of @list of @kind; for
 *   %AXIS_META_LIST_RECOMMENDED, the recommendations for the provider's
 *   title @id (%NULL for every other list).  Only asked for the lists the
 *   provider offers (@film_lists, @series_lists)
 * @genres: (nullable): the provider's genres of @kind (numbers and names)
 * @film_lists: the lists it offers for films (AXIS_META_LIST_BIT())
 * @series_lists: the lists it offers for series
 * @secondary: never chosen as the provider of a kind (the search box, the
 *   identification of the library, the setup guide and readiness take the
 *   first provider that is not secondary): asked only where a caller names
 *   it, and offered nowhere without its key (add-metadata-tvdb)
 * @attribution_url: (nullable): the address its notice links to, when its
 *   terms ask for a link
 * @pin_setting: (nullable): the key of one of its own @settings, a secret,
 *   that a login sends beside the key (a subscriber's PIN); read by the
 *   layer only
 * @login: (nullable): a provider whose key is exchanged for a token: gets a
 *   token with axis_meta_fetch_login() and reads it from the answer, into
 *   @token (transfer full, a secret).  The layer keeps it in memory only,
 *   sends it in @key_header after @key_prefix to @api_origin only, renews it
 *   after %AXIS_META_TOKEN_RENEW_S and after a 401 (one new login and one
 *   more try per request), and never stores, logs or shows it
 * @order: (nullable): every episode of the source of numbering @source
 *   (one of @sources) of the series @series_id, as its seasons with their
 *   episodes (AxisMetaSeason, each number once), reading every page
 * @sources: (nullable) (array zero-terminated=1): the sources of numbering
 *   @order serves (axis_meta_source_valid()); %NULL for a provider whose
 *   seasons are its own (@season)
 * @test: (nullable): asks something about no title, to check the key
 *   ("Test TheTVDB key"), and says in @summary (transfer full) what came back
 *
 * Everything a provider function returns has been through
 * axis_meta_title_check(), axis_meta_season_check(),
 * axis_meta_list_page_check() or axis_meta_genres_check() before it is
 * stored; a provider should check its own answers too.  @list and the fields
 * after it were added later: a table that does not set them offers no lists,
 * and one that does not set @secondary and those after it behaves as before.
 */
struct _AxisMetaProviderInfo
{
	const gchar *name;
	const gchar *label;
	guint        kinds;
	const gchar *attribution;
	const gchar *api_origin;
	const gchar *image_origin;
	gdouble      default_rate;
	guint        burst;
	const gchar *key_header;
	const gchar *key_prefix;
	const gchar *key_help;

	gboolean (*search)(const AxisMetaProviderInfo *self, AxisMetaFetch *fetch, AxisMetaKind kind,
	                   const gchar *text, gint year, guint page, GPtrArray **results, GError **error);
	gboolean (*title)(const AxisMetaProviderInfo *self, AxisMetaFetch *fetch, AxisMetaKind kind,
	                  const gchar *id, AxisMetaTitle **title, GError **error);
	gboolean (*season)(const AxisMetaProviderInfo *self, AxisMetaFetch *fetch, const gchar *series_id,
	                   gint number, AxisMetaSeason **season, GError **error);
	gboolean (*find_by_external)(const AxisMetaProviderInfo *self, AxisMetaFetch *fetch, AxisMetaKind kind,
	                             const gchar *scheme, const gchar *value, GPtrArray **results, GError **error);
	gchar   *(*image_path)(const AxisMetaProviderInfo *self, const gchar *ref, AxisMetaArtSize size);

	const AxisSettingInfo *settings;
	guint                  n_settings;
	void   (*prepare_settings)(const AxisMetaProviderInfo *self);

	gpointer     data;

	gboolean (*list)(const AxisMetaProviderInfo *self, AxisMetaFetch *fetch, AxisMetaKind kind, AxisMetaList list,
	                 const gchar *id, guint page, AxisMetaListPage **out, GError **error);
	gboolean (*genres)(const AxisMetaProviderInfo *self, AxisMetaFetch *fetch, AxisMetaKind kind,
	                   GPtrArray **genres, GError **error);
	guint        film_lists;
	guint        series_lists;

	gboolean     secondary;
	const gchar *attribution_url;
	const gchar *pin_setting;
	gboolean (*login)(const AxisMetaProviderInfo *self, AxisMetaFetch *fetch, gchar **token, GError **error);
	gboolean (*order)(const AxisMetaProviderInfo *self, AxisMetaFetch *fetch, const gchar *series_id,
	                  const gchar *source, GPtrArray **seasons, GError **error);
	const gchar *const *sources;
	gboolean (*test)(const AxisMetaProviderInfo *self, AxisMetaFetch *fetch, gchar **summary, GError **error);
};

/* A token got by a login is renewed after this long (TheTVDB's are valid a
 * month). */
#define AXIS_META_TOKEN_RENEW_S (25 * 86400)

/* The largest token a login may return (it goes in a request header). */
#define AXIS_META_TOKEN_MAX_BYTES 8192

G_END_DECLS

#endif /* AXIS_META_PROVIDER_H */
