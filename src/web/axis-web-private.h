/*
 * axis-web-private.h - shared by the files of the web shell
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef AXIS_WEB_PRIVATE_H
#define AXIS_WEB_PRIVATE_H

#include "web/axis-web.h"
#include "web/axis-web-attention.h"
#include "web/axis-web-limits.h"

G_BEGIN_DECLS

/* The shell serving @request: set by its middleware, %NULL outside it. */
AxisWeb *
axis_web_from_request(HtmxRequest *request);

AxisModuleRegistry *
axis_web_get_registry(AxisWeb *self);

AxisWebAuth *
axis_web_get_auth(AxisWeb *self);

/* The hub the pages open a stream on, or %NULL (set by the hub's
 * installation). */
AxisWebHub *
axis_web_get_hub(AxisWeb *self);

void
axis_web_set_hub(
	AxisWeb    *self,
	AxisWebHub *hub
);

/* The header widget, if a module set one. */
HtmxNode *
axis_web_build_header_widget(
	AxisWeb     *self,
	HtmxRequest *request
);

/* The footer's notices, if a module set them and it is on. */
HtmxNode *
axis_web_build_footer_notices(
	AxisWeb     *self,
	HtmxRequest *request
);

/* The title search's box, if the module that serves it set one and it is on. */
HtmxNode *
axis_web_build_search_box(
	AxisWeb     *self,
	HtmxRequest *request
);

/* ── The title actions (axis-web-actions.c) ───────────────────────── */

typedef struct _AxisWebActions   AxisWebActions;
typedef struct _AxisWebActionRun AxisWebActionRun;

AxisWebActions *
axis_web_actions_new(void);

void
axis_web_actions_free(AxisWebActions *self);

/* @sw: (transfer full) (nullable): the module's switch */
void
axis_web_actions_add(
	AxisWebActions           *self,
	AxisModuleSwitch         *sw,
	AxisWebActionCollectFunc  collect,
	AxisWebActionRenderFunc   render,
	GDestroyNotify            free_figures,
	gpointer                  user_data
);

AxisWebActions *
axis_web_get_actions(AxisWeb *self);

/*
 * Reads, on a worker thread, what every live part needs about @titles shown
 * for @place (a title's page or a list of cards).
 * Returns: (transfer full) (nullable): the run; %NULL when no part is
 *   registered (nothing is drawn then).  Free it with
 *   axis_web_actions_run_free() on the main loop.
 */
AxisWebActionRun *
axis_web_actions_collect(
	AxisWebActions     *self,
	const GPtrArray    *titles,
	AxisWebActionPlace  place
);

void
axis_web_actions_run_free(AxisWebActionRun *run);

/*
 * The area of @place for @title (a div, class title-actions, card-actions
 * or title-detail), or %NULL when no part drew anything or @run is %NULL.
 * Main loop.
 */
HtmxNode *
axis_web_actions_render(
	AxisWebActionRun    *run,
	HtmxRequest         *request,
	AxisWebActionPlace   place,
	const AxisMetaTitle *title
);

/* The bar above a list of @kind (%AXIS_WEB_ACTION_BAR: where the person's
 * Adds from it go), or %NULL.  Its place (the id of the module's form, and
 * the place of its notice) is axis_web_actions_bar_place().  Main loop. */
HtmxNode *
axis_web_actions_render_bar(
	AxisWebActionRun *run,
	HtmxRequest      *request,
	AxisMetaKind      kind
);

/* "add-defaults-movies", "add-defaults-series". */
const gchar *
axis_web_actions_bar_place(AxisMetaKind kind);

/* ── The dashboard (axis-web-dashboard.c) ─────────────────────────── */

typedef struct _AxisWebDashboardParts AxisWebDashboardParts;

AxisWebDashboardParts *
axis_web_dashboard_parts_new(AxisWeb *web);

/* Waits for the dashboards being built and for the reading on its thread
 * (called off first: the wait does not depend on the database), and takes
 * no more. */
void
axis_web_dashboard_parts_stop(AxisWebDashboardParts *self);

/* The figures may have changed: a started board reads them again soon.  The
 * board of what needs attention says so whenever it is told of anything. */
void
axis_web_dashboard_parts_changed(AxisWebDashboardParts *self);

/* The card the part added last says "Counting..." in until it is read. */
void
axis_web_dashboard_parts_set_card(
	AxisWebDashboardParts *self,
	AxisWebDashboardSlot   slot
);

void
axis_web_dashboard_parts_free(AxisWebDashboardParts *self);

/* @sw: (transfer full) (nullable): the module's switch; @admins_only: the
 * part is left out (nothing collected) of an other viewer's dashboard */
void
axis_web_dashboard_parts_add(
	AxisWebDashboardParts       *self,
	AxisModuleSwitch            *sw,
	gboolean                     admins_only,
	AxisWebDashboardCollectFunc  collect,
	AxisWebDashboardRenderFunc   render,
	GDestroyNotify               free_figures,
	gpointer                     user_data
);

AxisWebDashboardParts *
axis_web_get_dashboard_parts(AxisWeb *self);

/* The route of "/"; @user_data is the shell. */
HtmxResponse *
axis_web_dashboard_handle(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
);

/* ── What needs attention (axis-web-attention.c) ──────────────────── */

typedef struct _AxisWebAttention AxisWebAttention;

AxisWebAttention *
axis_web_attention_new(AxisWeb *web);

/* Ends the readings, waits for the one on its thread and for the posts in
 * flight, and takes no more.  Before the modules are stopped. */
void
axis_web_attention_stop(AxisWebAttention *self);

void
axis_web_attention_free(AxisWebAttention *self);

/* @sw: (transfer full) (nullable): the module's switch */
void
axis_web_attention_add_source(
	AxisWebAttention        *self,
	AxisModuleSwitch        *sw,
	const gchar             *name,
	AxisAttentionSourceFunc  func,
	gpointer                 user_data
);

/* The page, the bell's own request and the two posts. */
void
axis_web_attention_register(
	AxisWebAttention *self,
	AxisWeb          *web
);

AxisWebAttention *
axis_web_get_attention(AxisWeb *self);

/*
 * The bell for the header of the page of @request (which is at
 * @request_path), or %NULL for a user who can have no notice.  From memory:
 * no statement is run.  Asks for the page to follow the board's topic while
 * the board is read.  Main loop.
 */
HtmxNode *
axis_web_attention_bell_new(
	AxisWeb     *web,
	HtmxRequest *request,
	const gchar *request_path
);

/* The dashboard's card: the head of the list /attention shows the user of
 * @request, or %NULL for one who can have no notice.  From memory. */
HtmxNode *
axis_web_attention_card_new(
	AxisWeb     *web,
	HtmxRequest *request
);

/* The topics the page being built asked for (see axis_web_page_add_topic());
 * a %NULL-terminated vector owned by the request, or %NULL for none. */
const gchar *const *
axis_web_request_get_topics(HtmxRequest *request);

/* Whether any route registered through the shell matches @path. */
gboolean
axis_web_knows_path(
	AxisWeb     *self,
	const gchar *path
);

/* For the tests that walk the route table: every route registered through
 * the shell, as "<METHOD> <pattern>", or "<METHOD> public <pattern>" for one
 * that needs no session.  Transfer full (of gchar *). */
GPtrArray *
axis_web_list_routes(AxisWeb *self);

/*
 * Marks @response as having its own cache policy, so that the middleware
 * does not replace it with "no-store".  Static assets only.
 */
void
axis_web_keep_cache_policy(HtmxResponse *response);

/* The server's request limits (web/axis-web-limits.h), borrowed. */
AxisWebLimits *
axis_web_get_limits(AxisWeb *self);

/* Puts the shell's security headers and "no-store" on a message's response
 * headers (an answer written without the shell: a streaming one, or a
 * refusal of axis-web-limits.c). */
void
axis_web_replace_security_headers(SoupMessageHeaders *headers);

/* The static asset route handler. */
HtmxResponse *
axis_web_static_handle(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
);

/*
 * The three paths browsers and phones request at the site root without
 * being told to.  They are the only paths besides /healthz, /login,
 * /setup and /static/ that need no session.
 */
#define AXIS_WEB_FAVICON_PATH                  "/favicon.ico"
#define AXIS_WEB_TOUCH_ICON_PATH               "/apple-touch-icon.png"
#define AXIS_WEB_TOUCH_ICON_PRECOMPOSED_PATH   "/apple-touch-icon-precomposed.png"

/* Answers those three paths from the compiled-in assets. */
HtmxResponse *
axis_web_icon_handle(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
);

/* Registers the three public routes. */
void
axis_web_icons_register(AxisWeb *web);

/* ── The mark ─────────────────────────────────────────────────── */

/**
 * axis_web_logo_new:
 * @mask_id: the id of the mask that cuts the hub (unique in the page)
 * @css_class: (nullable): an extra class for the svg element
 *
 * The Axis mark as an inline svg built from element nodes, its geometry
 * in attributes and its colours in classes (logo-step-1 .. logo-step-3,
 * logo-line) that the theme sets.  The geometry is that of
 * data/web/icons/logo-mark.svg.
 *
 * Returns: (transfer full): the svg element
 */
HtmxNode *
axis_web_logo_new(
	const gchar *mask_id,
	const gchar *css_class
);

/**
 * axis_web_lockup_new:
 *
 * The mark, large, beside the name: the head of the login and setup
 * pages.
 *
 * Returns: (transfer full): a div
 */
HtmxNode *
axis_web_lockup_new(void);

/* ── Themes ───────────────────────────────────────────────────── */

/* The theme id written on the page of @request: the account's own, or
 * the default one when it has none or no longer exists; never %NULL. */
const gchar *
axis_web_request_theme_id(HtmxRequest *request);

G_END_DECLS

#endif /* AXIS_WEB_PRIVATE_H */
