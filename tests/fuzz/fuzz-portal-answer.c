/*
 * fuzz-portal-answer.c - libFuzzer target for axis-portal's readers of the
 * daemon's answers, and for the pages written from what they read
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * (add-request-portal, slice P3.)  The first byte chooses the reader; the
 * rest is the answer, as the daemon (or whoever stands where the daemon
 * should be) sent it.  Whatever it is, a reader must return without crashing
 * or allocating without bound, and what it accepts must keep the promises of
 * clients/portal/axis-portal-wire.h:
 *
 *  - an account's name has the form of a username, a proof and a request's
 *    identifier the form the daemon gives, a list's name its alphabet;
 *  - a text is valid UTF-8 within its length, with no control character;
 *  - a number is in its range, a season is listed once;
 *
 * and the page written from it (clients/portal/axis-portal-view.h) is one
 * document of valid UTF-8 in which nothing of the answer is markup: no
 * control character, as many '<' as '>', and no quotation mark that a
 * text could have closed an attribute with (checked by writing the text that
 * came out of the reader through the page's own escape, which must hold none
 * of the characters markup is made of).
 */

#include <glib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../../clients/portal/axis-portal-view.h"
#include "../../clients/portal/axis-portal-wire.h"

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void
check_username(const gchar *name)
{
	gsize length;
	gsize i;

	if (name == NULL)
		abort();
	length = strlen(name);
	if (length == 0 || length > 64)
		abort();
	for (i = 0; i < length; i++) {
		if (!g_ascii_islower(name[i]) && !g_ascii_isdigit(name[i]) && name[i] != '.' && name[i] != '_' && name[i] != '-')
			abort();
	}
}

/* A text a page shows: valid, within @max_chars, no control character. */
static void
check_text(
	const gchar *text,
	glong        max_chars
){
	const gchar *p;

	if (text == NULL || !g_utf8_validate(text, -1, NULL) || g_utf8_strlen(text, -1) > max_chars)
		abort();
	for (p = text; *p != '\0'; p = g_utf8_next_char(p)) {
		gunichar c = g_utf8_get_char(p);

		if (c < 0x20 || c == 0x7f || g_unichar_iscntrl(c))
			abort();
	}
}

/* What the escape writes of any text holds nothing markup is made of. */
static void
check_escape(const gchar *text)
{
	GString *out = g_string_new(NULL);
	const guchar *p;

	axis_portal_view_escape(out, text);
	if (!g_utf8_validate(out->str, (gssize)out->len, NULL))
		abort();
	for (p = (const guchar *)out->str; *p != '\0'; p++) {
		if (*p < 0x20 || *p == 0x7f || *p == '<' || *p == '>' || *p == '"' || *p == '\'' || *p == '`')
			abort();
	}
	g_string_free(out, TRUE);
}

/* A page: one document, nothing of the answer in it as markup. */
static void
check_page(gchar *html)
{
	const guchar *p;
	gsize opened = 0;
	gsize closed = 0;

	if (html == NULL || !g_utf8_validate(html, -1, NULL) || !g_str_has_prefix(html, "<!DOCTYPE html>") ||
	    !g_str_has_suffix(html, "</main></body></html>\n"))
		abort();
	for (p = (const guchar *)html; *p != '\0'; p++) {
		if ((*p < 0x20 && *p != '\n') || *p == 0x7f)
			abort();
		if (*p == '<')
			opened++;
		if (*p == '>')
			closed++;
	}
	if (opened != closed || strstr(html, "<script") != NULL || strstr(html, " on") == html)
		abort();
	g_free(html);
}

static void
check_quota(const AxisPortalQuota *quota)
{
	if (quota->limit > 1000000000 || quota->left > 1000000000 || quota->window_ends < 0)
		abort();
}

static void
check_title(
	const AxisPortalTitleInfo *title,
	gboolean                   of_list
){
	guint i;
	guint j;

	if (title->id < 1 || title->year < 0 || title->year > 9999 || (guint)title->kind > 1 || (guint)title->state > 7)
		abort();
	check_text(title->name, AXIS_PORTAL_WIRE_NAME_MAX_CHARS);
	check_text(title->overview, AXIS_PORTAL_WIRE_OVERVIEW_MAX_CHARS);
	check_escape(title->name);
	check_escape(title->overview);
	if (title->asked_by != NULL)
		check_username(title->asked_by);
	if (title->seasons != NULL && (of_list || title->kind != AXIS_PORTAL_KIND_IS_SERIES ||
	    title->seasons->len > AXIS_PORTAL_WIRE_MAX_TITLE_SEASONS))
		abort();
	for (i = 0; title->seasons != NULL && i < title->seasons->len; i++) {
		const AxisPortalSeasonInfo *season = &g_array_index(title->seasons, AxisPortalSeasonInfo, i);

		if (season->number < 0 || season->number > 10000 || season->episodes < 0 || season->aired < 0 ||
		    (guint)season->state > 7)
			abort();
		for (j = 0; j < i; j++) {
			if (g_array_index(title->seasons, AxisPortalSeasonInfo, j).number == season->number)
				abort();
		}
	}
}

static void
check_request(const AxisPortalRequestInfo *request)
{
	guint i;

	if (!axis_portal_wire_is_request_id(request->id) || request->title < 1 || (guint)request->kind > 1 ||
	    (guint)request->state > 7 || (guint)request->cause > 4 || request->seasons == NULL ||
	    request->seasons->len > 100 || request->asked_at < 0 || request->changed_at < 0)
		abort();
	check_text(request->name, AXIS_PORTAL_WIRE_NAME_MAX_CHARS);
	if (request->reason != NULL) {
		check_text(request->reason, AXIS_PORTAL_WIRE_REASON_MAX_CHARS);
		check_escape(request->reason);
	}
	for (i = 0; i < request->seasons->len; i++) {
		if (g_array_index(request->seasons, gint, i) < 0 || g_array_index(request->seasons, gint, i) > 10000)
			abort();
	}
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	static const AxisPortalWho who = { (gchar *)"robin", { 5, 3, 1900000000 }, { 10, 7, 1900000000 } };
	AxisPortalFrame frame;
	const gchar *body;
	gsize length;
	guint i;

	if (size < 1)
		return 0;
	body = (const gchar *)data + 1;
	length = size - 1;
	frame.who = &who;
	frame.csrf = "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz";

	switch ((guint)(guint8)(data[0] - '0') % 11) {
	case 0: {
		g_autoptr(AxisPortalHello) hello = axis_portal_wire_read_hello(body, length);

		if (hello == NULL)
			break;
		if (hello->lists == NULL || g_strv_length(hello->lists) > AXIS_PORTAL_WIRE_MAX_LISTS)
			abort();
		for (i = 0; hello->lists[i] != NULL; i++) {
			if (!axis_portal_wire_is_list(hello->lists[i]))
				abort();
		}
		check_page(axis_portal_view_home(&frame, hello, hello->movies ? AXIS_PORTAL_KIND_IS_MOVIE :
			AXIS_PORTAL_KIND_IS_SERIES, hello->lists[0], NULL));
		break;
	}
	case 1: {
		g_autoptr(AxisPortalLogin) login = axis_portal_wire_read_login(body, length);

		if (login == NULL)
			break;
		check_username(login->name);
		if (!axis_portal_wire_is_proof(login->proof) || login->expires_in < 1 ||
		    login->expires_in > AXIS_PORTAL_WIRE_MAX_EXPIRES_S ||
		    (login->device != NULL && !axis_portal_wire_is_device(login->device)))
			abort();
		break;
	}
	case 2: {
		g_autoptr(AxisPortalWho) read = axis_portal_wire_read_whoami(body, length);
		AxisPortalFrame own;

		if (read == NULL)
			break;
		check_username(read->name);
		check_quota(&read->movies);
		check_quota(&read->seasons);
		own.who = read;
		own.csrf = frame.csrf;
		check_page(axis_portal_view_message(&own, "A heading", "A text."));
		break;
	}
	case 3:
	case 4: {
		gboolean search = (guint)(guint8)(data[0] - '0') % 11 == 3;
		g_autoptr(AxisPortalTitles) titles = axis_portal_wire_read_titles(body, length, search);
		AxisPortalHello hello;
		gchar *lists[2] = { (gchar *)"popular", NULL };

		if (titles == NULL)
			break;
		if (titles->titles->len > AXIS_PORTAL_WIRE_MAX_TITLES || titles->page < 1 || (!search && titles->stale))
			abort();
		for (i = 0; i < titles->titles->len; i++)
			check_title(g_ptr_array_index(titles->titles, i), TRUE);
		memset(&hello, 0, sizeof(hello));
		hello.movies = TRUE;
		hello.series = TRUE;
		hello.lists = lists;
		if (search)
			check_page(axis_portal_view_search(&frame, "what \"was\" <typed>", -1, titles));
		else
			check_page(axis_portal_view_home(&frame, &hello, AXIS_PORTAL_KIND_IS_MOVIE, "popular", titles));
		break;
	}
	case 5: {
		g_autoptr(AxisPortalTitleInfo) title = axis_portal_wire_read_title(body, length);

		if (title == NULL)
			break;
		check_title(title, FALSE);
		check_page(axis_portal_view_title(&frame, title, NULL));
		check_page(axis_portal_view_title(NULL, title, NULL));
		break;
	}
	case 6: {
		g_autoptr(AxisPortalAsked) asked = axis_portal_wire_read_asked(body, length);
		AxisPortalTitleInfo title;

		if (asked == NULL)
			break;
		if ((guint)asked->outcome > 3 || asked->left_available->len + asked->left_requested->len > 100 ||
		    asked->followed > 100)
			abort();
		check_quota(&asked->movies);
		check_quota(&asked->seasons);
		if (asked->request != NULL)
			check_request(asked->request);
		memset(&title, 0, sizeof(title));
		title.id = 7;
		title.kind = AXIS_PORTAL_KIND_IS_SERIES;
		title.name = (gchar *)"A title";
		title.overview = (gchar *)"";
		check_page(axis_portal_view_title(&frame, &title, asked));
		break;
	}
	case 7: {
		g_autoptr(AxisPortalRequests) requests = axis_portal_wire_read_requests(body, length);

		if (requests == NULL)
			break;
		if (requests->requests->len > AXIS_PORTAL_WIRE_MAX_REQUESTS || requests->page < 1)
			abort();
		for (i = 0; i < requests->requests->len; i++)
			check_request(g_ptr_array_index(requests->requests, i));
		check_page(axis_portal_view_requests(&frame, requests, "A notice."));
		break;
	}
	case 8: {
		AxisPortalRefusal refusal;

		if (!axis_portal_wire_read_refusal(body, length, &refusal) &&
		    (refusal.why != AXIS_PORTAL_REFUSED_OTHER || refusal.wait_s != 0))
			abort();
		if (refusal.wait_s < 0 || refusal.wait_s > AXIS_PORTAL_WIRE_MAX_WAIT_S || (guint)refusal.why > 17)
			abort();
		break;
	}
	case 9:
		if (axis_portal_wire_read_empty(body, length) && length < 2)
			abort();
		break;
	default: {
		/* The escape, and the pages that say what a browser typed. */
		g_autofree gchar *text = g_strndup(body, length);

		check_escape(text);
		check_page(axis_portal_view_message(&frame, text, text));
		check_page(axis_portal_view_login(text, text, text));
		check_page(axis_portal_view_search(&frame, text, 1, NULL));
		break;
	}
	}
	return 0;
}
