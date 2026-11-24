/*
 * axis-web.h - the web shell: page layout, navigation, static assets,
 * security headers and error pages
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Feature code adds its routes through axis_web_get() and friends (usually
 * from AxisModuleInfo.register_routes) and renders every page with
 * axis_web_page_respond().  It never writes <html>, a security header or
 * an error page itself.
 *
 * Every route needs a logged-in user unless it is one of the few public
 * ones (see web/axis-web-auth.h); nothing has to be done to get that.
 * A route that only administrators may use is registered with
 * AXIS_WEB_ROUTE_ADMIN.  A form that posts back carries the CSRF token:
 * add axis_web_csrf_field() to it.  htmx requests carry the token by
 * themselves (from the page's body).
 */

#ifndef AXIS_WEB_H
#define AXIS_WEB_H

#include <glib.h>
#include <htmx-glib.h>
#include <json-glib/json-glib.h>

#include "core/axis-module.h"
#include "meta/axis-meta-model.h"
#include "web/axis-web-limits.h"
#include "web/axis-web-table.h"
#include "web/axis-web-text.h"

G_BEGIN_DECLS

/* Defined in web/axis-web-auth.h. */
typedef struct _AxisWebAuth AxisWebAuth;

/* Defined in web/axis-web-hub.h. */
typedef struct _AxisWebHub AxisWebHub;

/* The largest request body accepted, AXIS_WEB_MAX_REQUEST_BODY, is in
 * web/axis-web-limits.h, which this header includes. */

/**
 * AxisWebRouteFlags:
 * @AXIS_WEB_ROUTE_DEFAULT: a logged-in user is required
 * @AXIS_WEB_ROUTE_ADMIN: an administrator is required; anyone else
 *   gets 403
 * @AXIS_WEB_ROUTE_PUBLIC: no session is required.  Only accepted for
 *   /healthz, /login, /setup, the three well-known icon paths
 *   (/favicon.ico, /apple-touch-icon.png and
 *   /apple-touch-icon-precomposed.png) and the static assets; any other
 *   pattern is refused with a critical and not registered.  A public route that is
 *   not GET still needs the pre-session CSRF token.
 * @AXIS_WEB_ROUTE_WORKER: the worker API: a worker's token is required, and
 *   a session counts for nothing (see web/axis-web-auth.h).  Only accepted
 *   alone, for POST, and for patterns under the worker API's prefix
 *   (AXIS_WORKER_API_PREFIX); no route without it may be registered there.
 */
typedef enum
{
	AXIS_WEB_ROUTE_DEFAULT = 0,
	AXIS_WEB_ROUTE_ADMIN   = 1 << 0,
	AXIS_WEB_ROUTE_PUBLIC  = 1 << 1,
	AXIS_WEB_ROUTE_WORKER  = 1 << 2
} AxisWebRouteFlags;

/**
 * axis_web_new:
 * @server: the HTTP server; its middleware pipeline and router are used
 * @registry: (transfer none): the registered modules, which must outlive
 *   the result and must not change after this call
 * @auth: (transfer none): accounts and sessions; must outlive the result
 *
 * Installs the security-header and error-page middleware, the
 * authentication middleware, the static asset route, the dashboard and
 * the login, setup and account pages, then calls register_routes of
 * every module in @registry.  The result must not be freed while @server
 * may still be handling requests.
 *
 * Returns: (transfer full): the shell
 */
AxisWeb *
axis_web_new(
	HtmxServer         *server,
	AxisModuleRegistry *registry,
	AxisWebAuth        *auth
);

void
axis_web_free(AxisWeb *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisWeb, axis_web_free)

/**
 * axis_web_route:
 * @self: the shell
 * @method: the HTTP method (not %HTMX_METHOD_UNKNOWN)
 * @pattern: the htmx-glib route pattern (":name" segments, "*")
 * @callback: the handler
 * @user_data: for @callback
 *
 * Adds a route.  Use this rather than the router directly: the shell
 * keeps a table of routes to answer a known path with the wrong method
 * with 405 and an Allow header.
 */
void
axis_web_route(
	AxisWeb           *self,
	HtmxMethod         method,
	const gchar       *pattern,
	HtmxRouteCallback  callback,
	gpointer           user_data
);

void axis_web_get(AxisWeb *self, const gchar *pattern, HtmxRouteCallback callback, gpointer user_data);
void axis_web_post(AxisWeb *self, const gchar *pattern, HtmxRouteCallback callback, gpointer user_data);
void axis_web_put(AxisWeb *self, const gchar *pattern, HtmxRouteCallback callback, gpointer user_data);
void axis_web_patch(AxisWeb *self, const gchar *pattern, HtmxRouteCallback callback, gpointer user_data);
void axis_web_delete(AxisWeb *self, const gchar *pattern, HtmxRouteCallback callback, gpointer user_data);

/**
 * axis_web_route_full:
 * @self: the shell
 * @method: the HTTP method
 * @pattern: the route pattern
 * @flags: who may use the route
 * @callback: the handler
 * @user_data: for @callback
 *
 * As axis_web_route(), with access flags.  The handler is only called
 * once the request has passed them (and, for any method but GET and HEAD,
 * the CSRF check).
 */
void
axis_web_route_full(
	AxisWeb           *self,
	HtmxMethod         method,
	const gchar       *pattern,
	AxisWebRouteFlags  flags,
	HtmxRouteCallback  callback,
	gpointer           user_data
);

/* Keeps @data for as long as the shell's routes live (a route's user data
 * made for it), freed with @free when the shell is. */
void
axis_web_keep(
	AxisWeb        *self,
	gpointer        data,
	GDestroyNotify  free_data
);

/**
 * axis_web_request_get_username:
 *
 * Returns: (transfer none) (nullable): the logged-in user's name, or
 *   %NULL when the request has no session
 */
const gchar *
axis_web_request_get_username(HtmxRequest *request);

/* Returns: %TRUE if the request's user is an administrator */
gboolean
axis_web_request_is_admin(HtmxRequest *request);

/* The logged-in user's account id, 0 without a session. */
gint64
axis_web_request_get_user_id(HtmxRequest *request);

/* The request's user's own choices for Add from a list of @kind ("movies",
 * "series"): where it goes and how (a module's members: "root", "profile",
 * ...); %NULL for none chosen.  Transfer full. */
JsonObject *
axis_web_request_add_defaults(
	HtmxRequest *request,
	const gchar *kind
);

/* After axis_user_store_set_add_defaults() (@all: what it stored, every
 * kind's), on the main loop: @request's answer and the cached account
 * follow, so that the very next page, from any browser of this user, uses
 * them. */
void
axis_web_request_note_add_defaults(
	HtmxRequest *request,
	const gchar *all
);

/**
 * axis_web_request_get_client_address:
 *
 * Returns: (transfer none): the client's address, from the trusted
 *   proxies' headers where they apply
 */
const gchar *
axis_web_request_get_client_address(HtmxRequest *request);

/**
 * axis_web_csrf_field:
 *
 * Returns: (transfer full) (nullable): a hidden input carrying the
 *   session's CSRF token, for a form that posts; %NULL without a session
 */
HtmxNode *
axis_web_csrf_field(HtmxRequest *request);

/**
 * axis_web_page_respond:
 * @request: the request being answered
 * @title: the page title (plain text; escaped)
 * @content: (transfer none): the page's own content
 *
 * Renders @content in the common layout.  For a normal request that is a
 * complete document with a doctype, header, navigation and footer; for an
 * htmx request (HX-Request) it is only a title element and the content,
 * unless the request is a history restore, which gets the full document.
 * The status is 200; change it with htmx_response_set_status().
 *
 * Returns: (transfer full): the response
 */
HtmxResponse *
axis_web_page_respond(
	HtmxRequest *request,
	const gchar *title,
	HtmxNode    *content
);

/**
 * axis_web_page_intro:
 * @text: one or two plain sentences: what the page is for and why someone
 *   would care (plain text; escaped)
 *
 * The line directly under a page's heading, muted.  Every page of Media
 * management and of System opens with one.
 *
 * Returns: (transfer full): a <p class="page-intro">
 */
HtmxNode *
axis_web_page_intro(const gchar *text);

/**
 * axis_web_error_respond:
 * @request: the request being answered
 * @status: the HTTP status
 * @heading: the page title and heading (plain text)
 * @message: one sentence for the page (plain text)
 *
 * An error page in the common layout, with @status.
 *
 * Returns: (transfer full): the response
 */
HtmxResponse *
axis_web_error_respond(
	HtmxRequest *request,
	guint        status,
	const gchar *heading,
	const gchar *message
);

/**
 * axis_web_internal_error:
 * @request: the request being answered
 * @error: (nullable): what went wrong
 *
 * For a handler that cannot finish.  @error is written to the log with
 * the request line and never appears in the response, which is a generic
 * 500 page.
 *
 * Returns: (transfer full): the response
 */
HtmxResponse *
axis_web_internal_error(
	HtmxRequest  *request,
	const GError *error
);

/**
 * axis_web_page_add_topic:
 * @request: the request being answered
 * @topic: a valid hub topic (see web/axis-web-hub-topic.h)
 *
 * Asks for the page being built to follow @topic live.  Once the hub is
 * installed (web/axis-web-hub.h), a full page that was asked to follow
 * any topic opens one event stream for all of them, and its elements
 * react to the events with the htmx SSE extension's attributes (sse-swap,
 * hx-trigger="sse:name").  The page itself must be complete without the
 * stream.  Call it while building the page's content or header; a
 * request that is not a full page, has no session or has no hub ignores
 * it.
 */
void
axis_web_page_add_topic(
	HtmxRequest *request,
	const gchar *topic
);

/**
 * AxisWebHeaderWidgetFunc:
 * @request: the page's request (it has a session)
 * @user_data: (closure): as given to axis_web_set_header_widget()
 *
 * Returns: (transfer full) (nullable): an element for the header of a
 *   full page, such as a status indicator, or %NULL for none
 */
typedef HtmxNode *(*AxisWebHeaderWidgetFunc)(
	HtmxRequest *request,
	gpointer     user_data
);

/**
 * axis_web_set_header_widget:
 * @self: the shell
 * @func: (nullable): builds the widget; %NULL removes it
 * @user_data: for @func; must outlive the shell's requests
 *
 * One module's element shown in the header of every page for a logged-in
 * user.  Call from register_routes.
 */
void
axis_web_set_header_widget(
	AxisWeb                 *self,
	AxisWebHeaderWidgetFunc  func,
	gpointer                 user_data
);

/**
 * axis_web_set_footer_notices:
 * @self: the shell
 * @func: (nullable): builds the notices (the request may have no session);
 *   %NULL removes them
 * @user_data: for @func; must outlive the shell's requests
 *
 * One module's element shown in the footer of every full page, for anyone:
 * the details providers' notices (web/axis-web-meta.c).  Built on the main
 * loop, from what is in memory only.  Call from register_routes.
 */
void
axis_web_set_footer_notices(
	AxisWeb                 *self,
	AxisWebHeaderWidgetFunc  func,
	gpointer                 user_data
);

/* ── The dashboard ──────────────────────────────────────────────── */

/**
 * AxisWebDashboardSlot:
 * @AXIS_WEB_DASHBOARD_COMING_IN: what is coming in: downloads, imports,
 *   wanted titles (first, once the acquisition modules exist)
 * @AXIS_WEB_DASHBOARD_SPACE: the space on each volume and the collection
 * @AXIS_WEB_DASHBOARD_NOW: what is running
 * @AXIS_WEB_DASHBOARD_SAVINGS: what converting would save
 * @AXIS_WEB_DASHBOARD_SETUP: "Getting started": what is still needed to
 *   download anything; shown first, across the page
 *
 * The cards of the dashboard, in the order they are shown (Getting started
 * before all of them).  The card that lists what needs attention is the
 * shell's own and comes after Now: it is drawn from the board of
 * web/axis-web-attention.h, which a module fills through an attention
 * source (axis_web_add_attention_source()), never through a dashboard part.
 * A card nothing was added to is not shown at all.
 */
typedef enum
{
	AXIS_WEB_DASHBOARD_COMING_IN,
	AXIS_WEB_DASHBOARD_SPACE,
	AXIS_WEB_DASHBOARD_NOW,
	AXIS_WEB_DASHBOARD_SAVINGS,
	AXIS_WEB_DASHBOARD_SETUP
} AxisWebDashboardSlot;

#define AXIS_WEB_DASHBOARD_SLOTS 5

/* The dashboard being built for one request. */
typedef struct _AxisWebDashboard AxisWebDashboard;

/**
 * AxisWebDashboardCollectFunc:
 * @user_data: (closure): as given to axis_web_add_dashboard_part()
 *
 * Runs on a worker thread, never the main loop: reads what the part shows
 * (it may block on the database).  Must not touch anything that belongs to
 * the main loop.
 *
 * Returns: (transfer full) (nullable): the figures, for the render function
 */
typedef gpointer (*AxisWebDashboardCollectFunc)(gpointer user_data);

/**
 * AxisWebDashboardRenderFunc:
 * @board: the dashboard being built
 * @request: its request (it has a session)
 * @figures: (nullable): what the collect function returned (%NULL when the
 *   part has none)
 * @user_data: (closure): as given to axis_web_add_dashboard_part()
 *
 * Runs on the main loop: adds the part's pieces to the cards.
 */
typedef void (*AxisWebDashboardRenderFunc)(
	AxisWebDashboard *board,
	HtmxRequest      *request,
	gpointer          figures,
	gpointer          user_data
);

/**
 * axis_web_add_dashboard_part:
 * @self: the shell
 * @collect: (nullable): reads the part's figures on a worker thread
 * @render: builds the part's pieces on the main loop
 * @free_figures: (nullable): frees what @collect returned
 * @user_data: for both; must outlive the shell's requests
 *
 * One module's contribution to the dashboard.  Call from register_routes:
 * the part goes with its module (a module switched off contributes
 * nothing).
 */
void
axis_web_add_dashboard_part(
	AxisWeb                     *self,
	AxisWebDashboardCollectFunc  collect,
	AxisWebDashboardRenderFunc   render,
	GDestroyNotify               free_figures,
	gpointer                     user_data
);

/* As axis_web_add_dashboard_part(), for a part only administrators are
 * shown: for anyone else nothing of it is collected or rendered. */
void
axis_web_add_dashboard_part_for_admins(
	AxisWeb                     *self,
	AxisWebDashboardCollectFunc  collect,
	AxisWebDashboardRenderFunc   render,
	GDestroyNotify               free_figures,
	gpointer                     user_data
);

/**
 * axis_web_set_dashboard_part_card:
 * @self: the shell
 * @slot: the card the part adds its pieces to
 *
 * Says of the part added last where it shows: on a started dashboard
 * (axis_web_dashboard_start()) that card says "Counting..." for the part
 * until its figures have been read once.  A part that does not say shows
 * nothing until then.
 */
void
axis_web_set_dashboard_part_card(
	AxisWeb              *self,
	AxisWebDashboardSlot  slot
);

/* What an open dashboard listens to: published after every reading of the
 * figures, carrying nothing; the page then asks for its cards again. */
#define AXIS_WEB_DASHBOARD_TOPIC     "dashboard"
#define AXIS_WEB_DASHBOARD_EVENT     "dashboard"

/* The figures are read again no sooner than this after the reading before,
 * and at least this often. */
#define AXIS_WEB_DASHBOARD_MIN_GAP_S 5
#define AXIS_WEB_DASHBOARD_PERIOD_S  60

/**
 * axis_web_dashboard_start:
 * @self: the shell, with every module's routes registered
 *
 * From here on the dashboard is drawn from memory: the parts' figures are
 * read in the background (a timer on the main loop and a worker thread), at
 * once the first time, then when something happened, when a dashboard is
 * opened and every %AXIS_WEB_DASHBOARD_PERIOD_S, and no page view runs a
 * part's collect function.  axis_web_stop() ends it, without waiting on the
 * database.  A shell that is never started reads the parts at every view.
 */
void
axis_web_dashboard_start(AxisWeb *self);

/* For the tests: other waits (0: keep).  Before or after the start. */
void
axis_web_dashboard_set_tuning(
	AxisWeb *self,
	guint    min_gap_ms,
	guint    period_ms
);

/* Returns: how many readings of the figures have finished */
guint
axis_web_dashboard_get_readings(AxisWeb *self);

/**
 * axis_web_dashboard_add:
 * @board: the dashboard
 * @slot: the card
 * @position: where in the card (pieces are shown by ascending position)
 * @href: (nullable): the page behind the card; the card's heading links to
 *   the @href of its first piece that gives one
 * @piece: (transfer full): the piece; it states when its figures were taken
 */
void
axis_web_dashboard_add(
	AxisWebDashboard     *board,
	AxisWebDashboardSlot  slot,
	gint                  position,
	const gchar          *href,
	HtmxNode             *piece
);

/**
 * axis_web_dashboard_as_of:
 * @us: when the figures were taken (microseconds since 1970), or 0
 * @prefix: what precedes the time ("As of", "Checked")
 *
 * Returns: (transfer full): a small line for a piece: "As of <time>."
 */
HtmxNode *
axis_web_dashboard_as_of(
	const gchar *prefix,
	gint64       us
);

/* ── The Activity page's words ──────────────────────────────────── */

/**
 * AxisWebActivityItem:
 * @job_id, @kind, @key: the job (@key nullable)
 * @payload: (nullable): its payload (borrowed)
 * @words: (out): what it does, in a person's words ("Searching for Lantern
 *   Bay (2025), season 1"), set by its module; %NULL: its kind's label is
 *   shown
 * @href: (out) (nullable): the page of what it is for, a path on this site
 *
 * One job of work the Activity page lists, for its module to name.
 */
typedef struct
{
	gint64       job_id;
	const gchar *kind;
	const gchar *key;
	JsonObject  *payload;
	gchar       *words;
	gchar       *href;
} AxisWebActivityItem;

/**
 * AxisWebActivityWordsFunc:
 * @items: (element-type AxisWebActivityItem): the jobs of the kind, at once
 * @user_data: (closure): as given to axis_web_add_activity_words()
 *
 * Runs on a worker thread: names the jobs (it may read the database, one
 * statement for all of them).
 */
typedef void (*AxisWebActivityWordsFunc)(
	GPtrArray *items,
	gpointer   user_data
);

/* One module's words for the jobs of @kind, from register_routes: they go
 * with the module. */
void
axis_web_add_activity_words(
	AxisWeb                  *self,
	const gchar              *kind,
	AxisWebActivityWordsFunc  func,
	gpointer                  user_data
);

/* Every module's words over @items (worker thread). */
void
axis_web_activity_words(
	AxisWeb   *self,
	GPtrArray *items
);

/* ── The search box and the title actions ───────────────────────── */

/**
 * axis_web_set_search_box:
 * @self: the shell
 * @func: (nullable): builds the box; %NULL removes it
 * @user_data: for @func; must outlive the shell's requests
 *
 * The title search's box in the place the top bar keeps for it, on every
 * full page of a logged-in user.  Only the module that serves the search
 * sets it; it goes with that module (a module switched off has no box).
 * Call from register_routes.
 */
void
axis_web_set_search_box(
	AxisWeb                 *self,
	AxisWebHeaderWidgetFunc  func,
	gpointer                 user_data
);

/**
 * AxisWebTitleMoreFunc:
 * @request: the page's request
 * @kind: the title's kind
 * @title_id: the store's id of the title the page is about
 * @user_data: (closure): as given to axis_web_set_title_more()
 *
 * Runs on the main loop and must not block: a card about other titles like
 * this one (Discover's "More like this"), for a module's own page of a title
 * (a series' episodes page).  It reads nothing: the card loads itself.
 *
 * Returns: (transfer full) (nullable): the card, or %NULL for none
 */
typedef HtmxNode *(*AxisWebTitleMoreFunc)(
	HtmxRequest  *request,
	AxisMetaKind  kind,
	gint64        title_id,
	gpointer      user_data
);

/* Only the module that draws the card sets it (from register_routes); it
 * goes with that module. */
void
axis_web_set_title_more(
	AxisWeb              *self,
	AxisWebTitleMoreFunc  func,
	gpointer              user_data
);

/* The card for the page of @request, or %NULL when no module draws one or
 * it is switched off.  Transfer full.  Main loop. */
HtmxNode *
axis_web_title_more_new(
	HtmxRequest  *request,
	AxisMetaKind  kind,
	gint64        title_id
);

/**
 * AxisWebActionPlace:
 * @AXIS_WEB_ACTION_PAGE: the action area of a title's own page
 * @AXIS_WEB_ACTION_CARD: the corner of a title's card in a list of results
 * @AXIS_WEB_ACTION_DETAIL: on a title's own page, between its header and
 *   its facts: the module's own cards (drawn with the figures collected for
 *   %AXIS_WEB_ACTION_PAGE)
 * @AXIS_WEB_ACTION_NOTICE: on a title's own page, above everything: what
 *   stands in the way of it, said first (drawn with the figures collected
 *   for %AXIS_WEB_ACTION_PAGE)
 * @AXIS_WEB_ACTION_DISCOVER: the corner of a title's card on a Discover
 *   page: its state here, or, for a title the module does not have, the
 *   way to add it in one step (collected for a list of titles, as for
 *   %AXIS_WEB_ACTION_CARD).  A form there carries axis_web_back_field(), so
 *   that the add comes back to the page.
 * @AXIS_WEB_ACTION_BAR: above such a list (Discover's, the search results),
 *   once per kind: where the person's Adds from it go (drawn with the
 *   figures collected for the list; the title passed only names the kind,
 *   its @id is 0)
 */
typedef enum
{
	AXIS_WEB_ACTION_PAGE,
	AXIS_WEB_ACTION_CARD,
	AXIS_WEB_ACTION_DETAIL,
	AXIS_WEB_ACTION_NOTICE,
	AXIS_WEB_ACTION_DISCOVER,
	AXIS_WEB_ACTION_BAR
} AxisWebActionPlace;

/**
 * AxisWebActionCollectFunc:
 * @titles: (element-type AxisMetaTitle): the titles about to be shown (stored
 *   records: their @id is the store's)
 * @place: what they are shown for: %AXIS_WEB_ACTION_PAGE for a title's own
 *   page (one title; its action area and its detail are drawn from these
 *   figures), %AXIS_WEB_ACTION_CARD for a list of results
 * @user_data: (closure): as given to axis_web_add_title_actions()
 *
 * Runs on a worker thread, never the main loop: reads what the part needs
 * about all of @titles at once (whether each is wanted, in the library),
 * which it may block on.  Must not touch anything that belongs to the main
 * loop.
 *
 * Returns: (transfer full) (nullable): the figures, for the render function
 */
typedef gpointer (*AxisWebActionCollectFunc)(
	const GPtrArray    *titles,
	AxisWebActionPlace  place,
	gpointer            user_data
);

/**
 * AxisWebActionRenderFunc:
 * @request: the request being answered (it has a session)
 * @place: where the piece is drawn
 * @title: one of the titles @collect was given (for the title's page, the
 *   one title)
 * @figures: (nullable): what the collect function returned
 * @user_data: (closure): as given to axis_web_add_title_actions()
 *
 * Runs on the main loop and must not block: draws this module's piece for
 * @title in @place (a button, a state badge).  A form that posts uses
 * axis_web_csrf_field().
 *
 * Returns: (transfer full) (nullable): the piece, or %NULL for none
 */
typedef HtmxNode *(*AxisWebActionRenderFunc)(
	HtmxRequest         *request,
	AxisWebActionPlace   place,
	const AxisMetaTitle *title,
	gpointer             figures,
	gpointer             user_data
);

/**
 * axis_web_add_title_actions:
 * @self: the shell
 * @collect: (nullable): reads the part's figures on a worker thread
 * @render: draws the part's pieces on the main loop
 * @free_figures: (nullable): frees what @collect returned
 * @user_data: for both; must outlive the shell's requests
 *
 * One module's actions for a title ("Add", "Wanted", "In library"): shown
 * in the action area of a title's page and in the corner of its result
 * cards.  Call from register_routes: the part goes with its module.  With
 * no part, the search and title pages have no action area at all.
 */
void
axis_web_add_title_actions(
	AxisWeb                  *self,
	AxisWebActionCollectFunc  collect,
	AxisWebActionRenderFunc   render,
	GDestroyNotify            free_figures,
	gpointer                  user_data
);

/**
 * axis_web_stop:
 * @self: the shell
 *
 * Waits for the dashboard work in flight (main loop only).  Call before
 * the modules whose parts it may be running are stopped.
 */
void
axis_web_stop(AxisWeb *self);

/**
 * axis_web_asset_url:
 * @asset: a bundled asset, such as "axis.css" or "vendor/htmx.min.js"
 *
 * Returns: (transfer full): its versioned URL, "/static/<version>/<asset>"
 */
gchar *
axis_web_asset_url(const gchar *asset);

/**
 * axis_web_asset_version:
 *
 * Returns: (transfer none): the version segment of every asset URL: a
 *   hash of all bundled assets, computed when the binary was built
 */
const gchar *
axis_web_asset_version(void);

G_END_DECLS

#endif /* AXIS_WEB_H */
