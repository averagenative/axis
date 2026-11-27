/*
 * axis-portal-view.c - the pages of axis-portal, as text
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "axis-portal-view.h"

#include <string.h>

#include "axis-portal-form.h"
#include "api/axis-portal-proto.h"

/* ── Writing markup ─────────────────────────────────────────────── */

void
axis_portal_view_escape(
	GString     *out,
	const gchar *text
){
	g_autofree gchar *valid = NULL;
	const gchar *p;

	if (text == NULL)
		return;
	if (!g_utf8_validate(text, -1, NULL)) {
		valid = g_utf8_make_valid(text, -1);
		text = valid;
	}
	for (p = text; *p != '\0'; p = g_utf8_next_char(p)) {
		gunichar c = g_utf8_get_char(p);

		switch (c) {
		case '&':
			g_string_append(out, "&amp;");
			break;
		case '<':
			g_string_append(out, "&lt;");
			break;
		case '>':
			g_string_append(out, "&gt;");
			break;
		case '"':
			g_string_append(out, "&quot;");
			break;
		case '\'':
			g_string_append(out, "&#39;");
			break;
		case '`':
			g_string_append(out, "&#96;");
			break;
		case '\n':
		case '\r':
		case '\t':
			g_string_append_c(out, ' ');
			break;
		default:
			if (c < 0x20 || c == 0x7f || g_unichar_iscntrl(c))
				break;
			g_string_append_unichar(out, c);
			break;
		}
	}
}

/* <@tag class="@klass">@text</@tag>; @tag and @klass are literals. */
static void
element(
	GString     *out,
	const gchar *tag,
	const gchar *klass,
	const gchar *text
){
	g_string_append_printf(out, "<%s", tag);
	if (klass != NULL)
		g_string_append_printf(out, " class=\"%s\"", klass);
	g_string_append_c(out, '>');
	axis_portal_view_escape(out, text);
	g_string_append_printf(out, "</%s>", tag);
}

/* <input type="hidden" name="@name" value="@value">; @name is a literal. */
static void
hidden(
	GString     *out,
	const gchar *name,
	const gchar *value
){
	g_string_append_printf(out, "<input type=\"hidden\" name=\"%s\" value=\"", name);
	axis_portal_view_escape(out, value);
	g_string_append(out, "\">");
}

/* A time as "2031-05-17 00:00 UTC". */
static gchar *
time_words(gint64 seconds)
{
	g_autoptr(GDateTime) when = g_date_time_new_from_unix_utc(seconds);
	gchar *text = when != NULL ? g_date_time_format(when, "%Y-%m-%d %H:%M UTC") : NULL;

	return text != NULL ? text : g_strdup("an unknown time");
}

static gchar *
day_words(gint64 seconds)
{
	g_autoptr(GDateTime) when = g_date_time_new_from_unix_utc(seconds);
	gchar *text = when != NULL ? g_date_time_format(when, "%Y-%m-%d") : NULL;

	return text != NULL ? text : g_strdup("");
}

static const gchar *
kind_word(AxisPortalKind kind)
{
	return kind == AXIS_PORTAL_KIND_IS_SERIES ? "series" : "movie";
}

static const gchar *
kind_label(AxisPortalKind kind)
{
	return kind == AXIS_PORTAL_KIND_IS_SERIES ? "Series" : "Movie";
}

static const gchar *
title_state_label(AxisPortalTitleState state)
{
	switch (state) {
	case AXIS_PORTAL_TITLE_AVAILABLE:
		return "In the library";
	case AXIS_PORTAL_TITLE_PARTLY:
		return "Partly in the library";
	case AXIS_PORTAL_TITLE_DOWNLOADING:
		return "Downloading";
	case AXIS_PORTAL_TITLE_APPROVED:
		return "Approved";
	case AXIS_PORTAL_TITLE_REQUESTED:
		return "Requested";
	case AXIS_PORTAL_TITLE_DECLINED:
		return "Declined";
	case AXIS_PORTAL_TITLE_CLOSED:
		return "Closed to requests";
	case AXIS_PORTAL_TITLE_OPEN:
	default:
		return "Can be requested";
	}
}

/* The class of a state's badge: a literal of this file. */
static const gchar *
title_state_class(AxisPortalTitleState state)
{
	switch (state) {
	case AXIS_PORTAL_TITLE_AVAILABLE:
		return "badge good";
	case AXIS_PORTAL_TITLE_PARTLY:
	case AXIS_PORTAL_TITLE_DOWNLOADING:
	case AXIS_PORTAL_TITLE_APPROVED:
	case AXIS_PORTAL_TITLE_REQUESTED:
		return "badge busy";
	case AXIS_PORTAL_TITLE_DECLINED:
	case AXIS_PORTAL_TITLE_CLOSED:
		return "badge bad";
	case AXIS_PORTAL_TITLE_OPEN:
	default:
		return "badge";
	}
}

static const gchar *
request_state_label(AxisPortalRequestState state)
{
	switch (state) {
	case AXIS_PORTAL_REQUEST_APPROVED:
		return "Approved";
	case AXIS_PORTAL_REQUEST_DOWNLOADING:
		return "Downloading";
	case AXIS_PORTAL_REQUEST_PARTLY:
		return "Partly in the library";
	case AXIS_PORTAL_REQUEST_AVAILABLE:
		return "In the library";
	case AXIS_PORTAL_REQUEST_DECLINED:
		return "Declined";
	case AXIS_PORTAL_REQUEST_WITHDRAWN:
		return "Withdrawn";
	case AXIS_PORTAL_REQUEST_FAILED:
		return "Failed";
	case AXIS_PORTAL_REQUEST_WAITING:
	default:
		return "Waiting for approval";
	}
}

static const gchar *
request_state_class(AxisPortalRequestState state)
{
	switch (state) {
	case AXIS_PORTAL_REQUEST_AVAILABLE:
		return "badge good";
	case AXIS_PORTAL_REQUEST_DECLINED:
	case AXIS_PORTAL_REQUEST_FAILED:
		return "badge bad";
	case AXIS_PORTAL_REQUEST_WITHDRAWN:
		return "badge";
	default:
		return "badge busy";
	}
}

const gchar *
axis_portal_view_cause_words(AxisPortalCause cause)
{
	switch (cause) {
	case AXIS_PORTAL_CAUSE_NOT_ADDED:
		return "The title could not be added.";
	case AXIS_PORTAL_CAUSE_DOWNLOAD_FAILED:
		return "A download failed. Axis is looking again.";
	case AXIS_PORTAL_CAUSE_NEEDS_ADMIN:
		return "A download finished and needs the administrator.";
	case AXIS_PORTAL_CAUSE_REMOVED:
		return "The title was removed from Axis.";
	case AXIS_PORTAL_CAUSE_NONE:
	default:
		return "";
	}
}

/* "Popular" for "popular"; a name this file does not know is shown as it
 * is, with spaces for its underscores (the reader allows [a-z0-9_-] only). */
static gchar *
list_label(const gchar *list)
{
	static const gchar *const known[][2] = {
		{ "popular", "Popular" }, { "trending", "Trending" }, { "now", "Out now" }, { "upcoming", "Upcoming" },
		{ "top_rated", "Top rated" }
	};
	gchar *label;
	guint i;

	for (i = 0; i < G_N_ELEMENTS(known); i++) {
		if (strcmp(list, known[i][0]) == 0)
			return g_strdup(known[i][1]);
	}
	label = g_strdup(list);
	g_strdelimit(label, "_", ' ');
	return label;
}

/* "Season 2", or "Specials" for season 0. */
static void
season_name(
	GString *out,
	gint     number
){
	if (number == 0)
		g_string_append(out, "Specials");
	else
		g_string_append_printf(out, "Season %d", number);
}

/* "seasons 1, 2 and 5" of a list of numbers. */
static void
season_list(
	GString      *out,
	const GArray *seasons
){
	guint i;

	g_string_append(out, seasons->len == 1 ? "season " : "seasons ");
	for (i = 0; i < seasons->len; i++) {
		if (i > 0)
			g_string_append(out, i + 1 == seasons->len ? " and " : ", ");
		g_string_append_printf(out, "%d", g_array_index(seasons, gint, i));
	}
}

/* ── The frame of every page ────────────────────────────────────── */

/* What is left of one kind's quota. */
static void
quota_words(
	GString               *out,
	const gchar           *what,
	const AxisPortalQuota *quota
){
	if (quota->limit == 0)
		g_string_append_printf(out, "%s: none. ", what);
	else
		g_string_append_printf(out, "%s: %u of %u left. ", what, quota->left, quota->limit);
}

static void
page_begin(
	GString               *out,
	const gchar           *title,
	const AxisPortalFrame *frame
){
	g_string_append(out, "<!DOCTYPE html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">"
		"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
		"<meta name=\"referrer\" content=\"same-origin\"><title>");
	axis_portal_view_escape(out, title);
	g_string_append(out, " - Axis requests</title><link rel=\"stylesheet\" href=\"" AXIS_PORTAL_STYLE_PATH "\">"
		"</head><body><header class=\"top\">");
	if (frame == NULL || frame->who == NULL) {
		g_string_append(out, "<span class=\"brand\">Axis requests</span></header><main>");
		return;
	}
	g_string_append(out, "<a class=\"brand\" href=\"/\">Axis requests</a><nav><a href=\"/\">Browse</a>"
		"<a href=\"/requests\">My requests</a></nav>"
		"<form class=\"find\" method=\"get\" action=\"/search\"><label>Search "
		"<input type=\"search\" name=\"q\" maxlength=\"100\" required></label>"
		"<button type=\"submit\">Search</button></form><div class=\"who\">");
	element(out, "span", "name", frame->who->name);
	g_string_append(out, "<form method=\"post\" action=\"/logout\">");
	hidden(out, AXIS_PORTAL_CSRF_FIELD, frame->csrf);
	g_string_append(out, "<button type=\"submit\">Log out</button></form></div></header><p class=\"quota\">");
	quota_words(out, "Movies", &frame->who->movies);
	quota_words(out, "Seasons", &frame->who->seasons);
	if (frame->who->movies.limit > 0 || frame->who->seasons.limit > 0) {
		g_autofree gchar *ends = time_words(frame->who->movies.limit > 0 ? frame->who->movies.window_ends :
			frame->who->seasons.window_ends);

		g_string_append(out, "The count starts again at ");
		axis_portal_view_escape(out, ends);
		g_string_append_c(out, '.');
	}
	g_string_append(out, "</p><main>");
}

static gchar *
page_end(GString *out)
{
	g_string_append(out, "</main></body></html>\n");
	return g_string_free(out, FALSE);
}

/* ── Pages ──────────────────────────────────────────────────────── */

gchar *
axis_portal_view_login(
	const gchar *form_token,
	const gchar *next,
	const gchar *message
){
	GString *out = g_string_new(NULL);

	page_begin(out, "Log in", NULL);
	g_string_append(out, "<h1>Log in</h1>");
	if (message != NULL)
		element(out, "p", "notice", message);
	if (form_token != NULL) {
		g_string_append(out, "<form class=\"login\" method=\"post\" action=\"/login\">");
		hidden(out, AXIS_PORTAL_CSRF_FIELD, form_token);
		if (next != NULL)
			hidden(out, "next", next);
		g_string_append(out, "<label>Username <input type=\"text\" name=\"username\" autocomplete=\"username\" "
			"maxlength=\"64\" required autofocus></label>"
			"<label>Password <input type=\"password\" name=\"password\" autocomplete=\"current-password\" "
			"maxlength=\"1024\" required></label><button type=\"submit\">Log in</button></form>"
			"<p class=\"hint\">The administrator of this Axis gives you your username and password.</p>"
			/* No link and no address: the portal must not say where Axis is. */
			"<p class=\"hint\">This page is for people who ask for titles. Administrators log in at Axis "
			"itself, never here.</p>");
	}
	return page_end(out);
}

gchar *
axis_portal_view_message(
	const AxisPortalFrame *frame,
	const gchar           *heading,
	const gchar           *text
){
	GString *out = g_string_new(NULL);

	page_begin(out, heading, frame);
	element(out, "h1", NULL, heading);
	element(out, "p", "notice", text);
	/* Whoever it is for: a person who is logged in is not told to log in
	 * again, and "/" sends one who is not to the login. */
	if (frame == NULL || frame->who == NULL)
		g_string_append(out, "<p><a href=\"/\">Go to the request portal</a></p>");
	else
		g_string_append(out, "<p><a href=\"/\">Back to the lists</a></p>");
	return page_end(out);
}

/* One title of a list: its poster, its name, its state. */
static void
card(
	GString               *out,
	const AxisPortalTitleInfo *title
){
	g_string_append_printf(out, "<li class=\"card\"><a href=\"/title/%" G_GINT64_FORMAT "\">", title->id);
	if (title->has_poster) {
		g_string_append_printf(out, "<img src=\"/art/%" G_GINT64_FORMAT "\" width=\"154\" height=\"231\" "
			"loading=\"lazy\" alt=\"", title->id);
		axis_portal_view_escape(out, title->name);
		g_string_append(out, "\">");
	} else {
		g_string_append(out, "<span class=\"noposter\">No picture</span>");
	}
	element(out, "span", "name", title->name);
	g_string_append(out, "</a><span class=\"facts\">");
	g_string_append(out, kind_label(title->kind));
	if (title->year > 0)
		g_string_append_printf(out, ", %d", title->year);
	g_string_append(out, "</span>");
	element(out, "span", title_state_class(title->state), title_state_label(title->state));
	if (title->asked_by != NULL) {
		g_string_append(out, "<span class=\"asked\">Asked by ");
		axis_portal_view_escape(out, title->asked_by);
		g_string_append(out, "</span>");
	}
	g_string_append(out, "</li>");
}

static void
cards(
	GString                *out,
	const AxisPortalTitles *titles
){
	guint i;

	g_string_append(out, "<ul class=\"cards\">");
	for (i = 0; i < titles->titles->len; i++)
		card(out, g_ptr_array_index(titles->titles, i));
	g_string_append(out, "</ul>");
}

gchar *
axis_portal_view_home(
	const AxisPortalFrame  *frame,
	const AxisPortalHello  *hello,
	AxisPortalKind          kind,
	const gchar            *list,
	const AxisPortalTitles *titles
){
	GString *out = g_string_new(NULL);
	guint i;

	page_begin(out, "Browse", frame);
	g_string_append(out, "<h1>Browse</h1>");
	if (!hello->movies && !hello->series) {
		element(out, "p", "notice", "Nothing can be requested at the moment.");
		return page_end(out);
	}
	g_string_append(out, "<nav class=\"tabs\">");
	if (hello->movies)
		g_string_append_printf(out, "<a%s href=\"/?kind=movie\">Movies</a>",
			kind == AXIS_PORTAL_KIND_IS_MOVIE ? " class=\"here\"" : "");
	if (hello->series)
		g_string_append_printf(out, "<a%s href=\"/?kind=series\">Series</a>",
			kind == AXIS_PORTAL_KIND_IS_SERIES ? " class=\"here\"" : "");
	g_string_append(out, "</nav>");
	if (hello->lists[0] == NULL || list == NULL) {
		element(out, "p", "notice", "There are no lists to browse yet. Search for a title by its name.");
		return page_end(out);
	}
	g_string_append(out, "<nav class=\"tabs lists\">");
	for (i = 0; hello->lists[i] != NULL; i++) {
		g_autofree gchar *label = list_label(hello->lists[i]);

		/* A list's name is of [a-z0-9_-]: the reader refused any other. */
		g_string_append_printf(out, "<a%s href=\"/?kind=%s&amp;list=", strcmp(hello->lists[i], list) == 0 ?
			" class=\"here\"" : "", kind_word(kind));
		axis_portal_view_escape(out, hello->lists[i]);
		g_string_append(out, "\">");
		axis_portal_view_escape(out, label);
		g_string_append(out, "</a>");
	}
	g_string_append(out, "</nav>");
	if (titles == NULL || titles->titles->len == 0) {
		element(out, "p", "notice", "This list holds nothing at the moment.");
	} else {
		cards(out, titles);
		g_string_append(out, "<nav class=\"pages\">");
		if (titles->page > 1) {
			g_string_append_printf(out, "<a href=\"/?kind=%s&amp;list=", kind_word(kind));
			axis_portal_view_escape(out, list);
			g_string_append_printf(out, "&amp;page=%u\">Earlier</a>", titles->page - 1);
		}
		/* No link to a page that the portal would refuse. */
		if (titles->more && titles->page < AXIS_PORTAL_MAX_PAGE) {
			g_string_append_printf(out, "<a href=\"/?kind=%s&amp;list=", kind_word(kind));
			axis_portal_view_escape(out, list);
			g_string_append_printf(out, "&amp;page=%u\">More</a>", titles->page + 1);
		}
		g_string_append(out, "</nav>");
	}
	return page_end(out);
}

gchar *
axis_portal_view_search(
	const AxisPortalFrame  *frame,
	const gchar            *query,
	gint                    kind,
	const AxisPortalTitles *titles
){
	GString *out = g_string_new(NULL);

	page_begin(out, "Search", frame);
	g_string_append(out, "<h1>Search</h1><form class=\"search\" method=\"get\" action=\"/search\">"
		"<label>Name <input type=\"search\" name=\"q\" maxlength=\"100\" required value=\"");
	axis_portal_view_escape(out, query);
	g_string_append(out, "\"></label><label>Kind <select name=\"kind\">");
	g_string_append_printf(out, "<option value=\"any\"%s>Movies and series</option>", kind < 0 ? " selected" : "");
	g_string_append_printf(out, "<option value=\"movie\"%s>Movies</option>", kind == 0 ? " selected" : "");
	g_string_append_printf(out, "<option value=\"series\"%s>Series</option>", kind == 1 ? " selected" : "");
	g_string_append(out, "</select></label><button type=\"submit\">Search</button></form>");
	if (titles != NULL) {
		if (titles->stale)
			element(out, "p", "notice", "The details provider did not answer. These are results from before, and "
				"they may be incomplete.");
		if (titles->titles->len == 0) {
			element(out, "p", "notice", "Nothing was found.");
		} else {
			cards(out, titles);
			if (titles->more && query != NULL && titles->page < AXIS_PORTAL_SEARCH_MAX_PAGE) {
				g_autofree gchar *escaped = g_uri_escape_string(query, NULL, FALSE);

				g_string_append(out, "<nav class=\"pages\"><a href=\"/search?q=");
				axis_portal_view_escape(out, escaped);
				g_string_append_printf(out, "&amp;kind=%s&amp;page=%u\">More</a></nav>",
					kind < 0 ? "any" : kind == 1 ? "series" : "movie", titles->page + 1);
			}
		}
	}
	return page_end(out);
}

/* Whether a title or a season in @state can be asked for. */
static gboolean
can_ask(AxisPortalTitleState state)
{
	return state == AXIS_PORTAL_TITLE_OPEN || state == AXIS_PORTAL_TITLE_DECLINED;
}

/* Whether an existing request for it can be followed. */
static gboolean
can_follow(AxisPortalTitleState state)
{
	return state == AXIS_PORTAL_TITLE_REQUESTED || state == AXIS_PORTAL_TITLE_APPROVED ||
		state == AXIS_PORTAL_TITLE_DOWNLOADING || state == AXIS_PORTAL_TITLE_PARTLY;
}

/* What a request that was just made answered. */
static void
asked_notice(
	GString               *out,
	const AxisPortalAsked *asked
){
	g_string_append(out, "<div class=\"notice\"><p>");
	switch (asked->outcome) {
	case AXIS_PORTAL_OUTCOME_MADE:
		g_string_append(out, asked->request != NULL && asked->request->state != AXIS_PORTAL_REQUEST_WAITING ?
			"Your request is made and approved." : "Your request is made. It waits for the administrator.");
		break;
	case AXIS_PORTAL_OUTCOME_FOLLOWED:
		g_string_append(out, "Someone asked for this already. You now follow that request.");
		break;
	case AXIS_PORTAL_OUTCOME_AVAILABLE:
		g_string_append(out, "This is in the library already.");
		break;
	case AXIS_PORTAL_OUTCOME_UNCHANGED:
	default:
		g_string_append(out, "You asked for this already.");
		break;
	}
	g_string_append(out, "</p>");
	if (asked->left_available->len > 0) {
		g_string_append(out, "<p>Left out, because in the library already: ");
		season_list(out, asked->left_available);
		g_string_append(out, ".</p>");
	}
	if (asked->left_requested->len > 0) {
		g_string_append(out, "<p>Left out, because requested already: ");
		season_list(out, asked->left_requested);
		g_string_append(out, ".</p>");
	}
	g_string_append(out, "<p><a href=\"/requests\">My requests</a></p></div>");
}

/* What is left for @kind, in a sentence under the form. */
static void
left_words(
	GString               *out,
	const AxisPortalFrame *frame,
	AxisPortalKind         kind
){
	const AxisPortalQuota *quota;
	g_autofree gchar *ends = NULL;

	if (frame == NULL || frame->who == NULL)
		return;
	quota = kind == AXIS_PORTAL_KIND_IS_SERIES ? &frame->who->seasons : &frame->who->movies;
	ends = time_words(quota->window_ends);
	g_string_append(out, "<p class=\"left\">");
	if (kind == AXIS_PORTAL_KIND_IS_SERIES)
		g_string_append_printf(out, "You can ask for %u more of %u seasons until ", quota->left, quota->limit);
	else
		g_string_append_printf(out, "You can ask for %u more of %u movies until ", quota->left, quota->limit);
	axis_portal_view_escape(out, ends);
	g_string_append(out, ". Following a request that exists does not count.</p>");
}

static void
form_begin(
	GString               *out,
	const AxisPortalFrame *frame,
	const AxisPortalTitleInfo *title
){
	g_string_append_printf(out, "<form class=\"ask\" method=\"post\" action=\"/title/%" G_GINT64_FORMAT "/request\">",
		title->id);
	hidden(out, AXIS_PORTAL_CSRF_FIELD, frame != NULL ? frame->csrf : NULL);
	g_string_append_printf(out, "<input type=\"hidden\" name=\"kind\" value=\"%s\">", kind_word(title->kind));
}

gchar *
axis_portal_view_title(
	const AxisPortalFrame *frame,
	const AxisPortalTitleInfo *title,
	const AxisPortalAsked *asked
){
	GString *out = g_string_new(NULL);
	gboolean allowed = frame != NULL && frame->who != NULL &&
		(title->kind == AXIS_PORTAL_KIND_IS_SERIES ? frame->who->seasons.limit : frame->who->movies.limit) > 0;
	guint askable = 0;
	guint i;

	page_begin(out, title->name, frame);
	if (asked != NULL)
		asked_notice(out, asked);
	g_string_append(out, "<article class=\"title\">");
	if (title->has_poster) {
		g_string_append_printf(out, "<img class=\"poster\" src=\"/art/%" G_GINT64_FORMAT "\" width=\"154\" "
			"height=\"231\" alt=\"", title->id);
		axis_portal_view_escape(out, title->name);
		g_string_append(out, "\">");
	}
	g_string_append(out, "<div class=\"about\">");
	element(out, "h1", NULL, title->name);
	g_string_append(out, "<p class=\"facts\">");
	g_string_append(out, kind_label(title->kind));
	if (title->year > 0)
		g_string_append_printf(out, ", %d", title->year);
	g_string_append(out, " ");
	element(out, "span", title_state_class(title->state), title_state_label(title->state));
	if (title->asked_by != NULL) {
		g_string_append(out, " <span class=\"asked\">Asked by ");
		axis_portal_view_escape(out, title->asked_by);
		g_string_append(out, "</span>");
	}
	g_string_append(out, "</p>");
	element(out, "p", "overview", title->overview);

	if (title->kind == AXIS_PORTAL_KIND_IS_MOVIE) {
		if (!allowed) {
			element(out, "p", "left", "You cannot ask for movies.");
		} else if (can_ask(title->state) || can_follow(title->state)) {
			form_begin(out, frame, title);
			g_string_append_printf(out, "<button type=\"submit\">%s</button></form>",
				can_ask(title->state) ? "Request this movie" : "Follow this request");
			left_words(out, frame, title->kind);
		}
		g_string_append(out, "</div></article>");
		return page_end(out);
	}

	for (i = 0; title->seasons != NULL && i < title->seasons->len; i++) {
		if (can_ask(g_array_index(title->seasons, AxisPortalSeasonInfo, i).state))
			askable++;
	}
	if (!allowed) {
		element(out, "p", "left", "You cannot ask for series.");
	} else if (askable > 0 || ((title->seasons == NULL || title->seasons->len == 0) && can_ask(title->state))) {
		form_begin(out, frame, title);
		g_string_append(out, "<fieldset><legend>Which seasons?</legend>"
			"<label><input type=\"radio\" name=\"what\" value=\"all\" checked> The whole series</label>"
			"<label><input type=\"radio\" name=\"what\" value=\"first\"> The first season</label>"
			"<label><input type=\"radio\" name=\"what\" value=\"latest\"> The latest season</label>");
		if (askable > 0)
			g_string_append(out, "<label><input type=\"radio\" name=\"what\" value=\"picked\"> The seasons ticked "
				"below</label>");
		g_string_append(out, "</fieldset>");
	}
	if (title->seasons != NULL && title->seasons->len > 0) {
		g_string_append(out, "<table class=\"seasons\"><thead><tr><th>Season</th><th>Episodes aired</th>"
			"<th>State</th><th>Ask</th></tr></thead><tbody>");
		for (i = 0; i < title->seasons->len; i++) {
			const AxisPortalSeasonInfo *season = &g_array_index(title->seasons, AxisPortalSeasonInfo, i);

			g_string_append(out, "<tr><td>");
			season_name(out, season->number);
			g_string_append_printf(out, "</td><td>%d of %d</td><td>", season->aired, season->episodes);
			element(out, "span", title_state_class(season->state), title_state_label(season->state));
			g_string_append(out, "</td><td>");
			/* Only a season that can be requested has a box. */
			if (allowed && can_ask(season->state)) {
				g_string_append_printf(out, "<label><input type=\"checkbox\" name=\"season\" value=\"%d\"> ",
					season->number);
				season_name(out, season->number);
				g_string_append(out, "</label>");
			}
			g_string_append(out, "</td></tr>");
		}
		g_string_append(out, "</tbody></table>");
	}
	if (allowed && (askable > 0 || ((title->seasons == NULL || title->seasons->len == 0) && can_ask(title->state)))) {
		g_string_append(out, "<button type=\"submit\">Request</button></form>");
		left_words(out, frame, title->kind);
	}
	g_string_append(out, "</div></article>");
	return page_end(out);
}

gchar *
axis_portal_view_requests(
	const AxisPortalFrame    *frame,
	const AxisPortalRequests *requests,
	const gchar              *notice
){
	GString *out = g_string_new(NULL);
	guint i;

	page_begin(out, "My requests", frame);
	g_string_append(out, "<h1>My requests</h1>");
	if (notice != NULL)
		element(out, "p", "notice", notice);
	if (requests->requests->len == 0) {
		element(out, "p", "notice", requests->page > 1 ? "There are no more requests." :
			"You have asked for nothing yet. Browse or search for a title, then ask for it on its page.");
	} else {
		g_string_append(out, "<table class=\"requests\"><thead><tr><th>Title</th><th>What</th><th>State</th>"
			"<th>Asked</th><th></th></tr></thead><tbody>");
	}
	for (i = 0; i < requests->requests->len; i++) {
		const AxisPortalRequestInfo *request = g_ptr_array_index(requests->requests, i);
		g_autofree gchar *day = day_words(request->asked_at);

		g_string_append_printf(out, "<tr><td><a href=\"/title/%" G_GINT64_FORMAT "\">", request->title);
		axis_portal_view_escape(out, request->name);
		g_string_append(out, "</a>");
		if (request->year > 0)
			g_string_append_printf(out, " (%d)", request->year);
		g_string_append(out, "</td><td>");
		if (request->kind == AXIS_PORTAL_KIND_IS_SERIES && request->seasons->len > 0) {
			g_string_append(out, "Series, ");
			season_list(out, request->seasons);
		} else {
			g_string_append(out, kind_label(request->kind));
		}
		if (!request->made)
			g_string_append(out, " <span class=\"followed\">(you follow this request)</span>");
		g_string_append(out, "</td><td>");
		element(out, "span", request_state_class(request->state), request_state_label(request->state));
		if (request->counts && request->of > 0)
			g_string_append_printf(out, " <span class=\"have\">%u of %u episodes</span>", request->have, request->of);
		if (request->state == AXIS_PORTAL_REQUEST_FAILED && request->cause != AXIS_PORTAL_CAUSE_NONE) {
			g_string_append_c(out, ' ');
			element(out, "span", "cause", axis_portal_view_cause_words(request->cause));
		}
		if (request->state == AXIS_PORTAL_REQUEST_DECLINED && request->reason != NULL) {
			g_string_append(out, " <span class=\"reason\">The administrator wrote: ");
			axis_portal_view_escape(out, request->reason);
			g_string_append(out, "</span>");
		}
		g_string_append(out, "</td><td>");
		axis_portal_view_escape(out, day);
		g_string_append(out, "</td><td>");
		/* Only who made a request can withdraw it, and only while it waits. */
		if (request->made && request->state == AXIS_PORTAL_REQUEST_WAITING) {
			g_string_append(out, "<form method=\"post\" action=\"/requests/");
			axis_portal_view_escape(out, request->id);
			g_string_append(out, "/withdraw\">");
			hidden(out, AXIS_PORTAL_CSRF_FIELD, frame != NULL ? frame->csrf : NULL);
			g_string_append(out, "<button type=\"submit\">Withdraw</button></form>");
		}
		g_string_append(out, "</td></tr>");
	}
	if (requests->requests->len > 0)
		g_string_append(out, "</tbody></table>");
	g_string_append(out, "<nav class=\"pages\">");
	if (requests->page > 1)
		g_string_append_printf(out, "<a href=\"/requests?page=%u\">Later</a>", requests->page - 1);
	if (requests->more && requests->page < AXIS_PORTAL_MAX_PAGE)
		g_string_append_printf(out, "<a href=\"/requests?page=%u\">Earlier</a>", requests->page + 1);
	g_string_append(out, "</nav>");
	return page_end(out);
}

/* ── The stylesheet ─────────────────────────────────────────────── */

const gchar *
axis_portal_view_style(void)
{
	/* Dark by default; light when the browser asks for it. */
	return
		":root{--bg:#14171c;--panel:#1d2229;--line:#2e3540;--text:#e6e9ee;--dim:#9aa4b2;--link:#7fb4ff;"
		"--good:#2f6f47;--busy:#6a5a1f;--bad:#7a2f35}\n"
		"@media (prefers-color-scheme: light){:root{--bg:#f6f7f9;--panel:#fff;--line:#d5dae1;--text:#1b1f24;"
		"--dim:#5b6573;--link:#1856b8;--good:#bfe6cc;--busy:#f1e3a8;--bad:#f3c1c5}}\n"
		"*{box-sizing:border-box}\n"
		"body{margin:0;background:var(--bg);color:var(--text);font:16px/1.5 system-ui,sans-serif}\n"
		"a{color:var(--link)}\n"
		".top{display:flex;flex-wrap:wrap;gap:.75rem 1.25rem;align-items:center;padding:.75rem 1rem;"
		"background:var(--panel);border-bottom:1px solid var(--line)}\n"
		".top nav{display:flex;gap:1rem}\n"
		".top form{display:flex;gap:.5rem;align-items:center;margin:0}\n"
		".brand{font-weight:700;color:var(--text);text-decoration:none}\n"
		".who{display:flex;gap:.75rem;align-items:center;margin-left:auto}\n"
		".quota{margin:0;padding:.4rem 1rem;color:var(--dim);font-size:.9rem;border-bottom:1px solid var(--line)}\n"
		"main{max-width:70rem;margin:0 auto;padding:1rem}\n"
		"input,select,button{font:inherit;color:inherit;background:var(--bg);border:1px solid var(--line);"
		"border-radius:.3rem;padding:.35rem .6rem}\n"
		"button{background:var(--panel);cursor:pointer}\n"
		".login{display:grid;gap:.75rem;max-width:22rem}\n"
		".login label,.search label{display:grid;gap:.25rem}\n"
		".search{display:flex;flex-wrap:wrap;gap:.75rem;align-items:end;margin-bottom:1rem}\n"
		".notice{background:var(--panel);border:1px solid var(--line);border-radius:.3rem;padding:.6rem .8rem}\n"
		"div.notice p{margin:.2rem 0}\n"
		".hint,.left,.facts,.asked,.have,.followed{color:var(--dim)}\n"
		".tabs{display:flex;flex-wrap:wrap;gap:.5rem;margin:.5rem 0}\n"
		".tabs a{padding:.25rem .7rem;border:1px solid var(--line);border-radius:1rem;text-decoration:none}\n"
		".tabs a.here{background:var(--panel);color:var(--text)}\n"
		".cards{list-style:none;margin:1rem 0;padding:0;display:grid;gap:1rem;"
		"grid-template-columns:repeat(auto-fill,minmax(10rem,1fr))}\n"
		".card{display:grid;gap:.25rem;align-content:start;background:var(--panel);border:1px solid var(--line);"
		"border-radius:.4rem;padding:.6rem}\n"
		".card a{display:grid;gap:.4rem;text-decoration:none}\n"
		".card img,.poster{max-width:100%;height:auto;border-radius:.3rem}\n"
		".noposter{display:grid;place-items:center;aspect-ratio:2/3;background:var(--bg);color:var(--dim);"
		"border-radius:.3rem}\n"
		".badge{display:inline-block;padding:.05rem .5rem;border-radius:.8rem;border:1px solid var(--line);"
		"font-size:.85rem}\n"
		".badge.good{background:var(--good)}\n.badge.busy{background:var(--busy)}\n.badge.bad{background:var(--bad)}\n"
		".title{display:flex;flex-wrap:wrap;gap:1.25rem;align-items:flex-start}\n"
		".about{flex:1 1 20rem}\n"
		".ask fieldset{display:grid;gap:.3rem;border:1px solid var(--line);border-radius:.3rem;margin:0 0 .75rem}\n"
		"table{border-collapse:collapse;width:100%;margin:.75rem 0}\n"
		"th,td{text-align:left;padding:.4rem .6rem;border-bottom:1px solid var(--line);vertical-align:top}\n"
		".cause,.reason{display:block;color:var(--dim)}\n"
		".pages{display:flex;gap:1rem;margin:1rem 0}\n";
}
