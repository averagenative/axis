/*
 * axis-web-compat-pages.c - the Compatibility pages (administrators only)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Names from the media server (devices, libraries, folders, accounts) are
 * text content inside <bdi>, escaped; links carry ids and fixed names
 * only.  The token is never rendered: it is not read here at all.
 */

#include "web/axis-web-compat-private.h"

#include <string.h>

#include "compat/axis-compat-notify.h"
#include "db/axis-db-error.h"
#include "library/axis-library-name.h"
#include "web/axis-web-notice.h"
#include "web/axis-web-place.h"

/* ── Pieces ─────────────────────────────────────────────────────── */

static HtmxResponse *
not_found(HtmxRequest *request)
{
	return axis_web_error_respond(request, 404, "Not found", "There is nothing at this address.");
}

static HtmxNode *
bdi_text(const gchar *text)
{
	HtmxBdi *bdi = htmx_bdi_new();

	htmx_node_set_text_content(HTMX_NODE(bdi), text != NULL ? text : "");
	return HTMX_NODE(bdi);
}

static HtmxNode *
text_cell(
	const gchar *css,
	const gchar *text
){
	HtmxTd *cell = htmx_td_new();

	if (css != NULL)
		htmx_element_add_class(HTMX_ELEMENT(cell), css);
	htmx_node_set_text_content(HTMX_NODE(cell), text);
	return HTMX_NODE(cell);
}

/* A cell holding a name from the server. */
static HtmxNode *
name_cell(const gchar *text)
{
	HtmxTd *cell = htmx_td_new();

	BROWSER_ADD(cell, bdi_text(text));
	return HTMX_NODE(cell);
}

static const gchar *
device_label(
	const gchar *name,
	const gchar *client_id
){
	return name != NULL && name[0] != '\0' ? name : client_id;
}

static HtmxNode *
verdict_badge(AxisCompatVerdict verdict)
{
	return axis_web_browser_badge(axis_compat_verdict_label(verdict),
		verdict == AXIS_VERDICT_SAFE ? "badge-succeeded" :
		(verdict == AXIS_VERDICT_RISKY ? "badge-failed" : "badge-warn"));
}

static const gchar *
kind_label(gint kind)
{
	return kind == AXIS_COMPAT_STREAM_VIDEO ? "Video" : (kind == AXIS_COMPAT_STREAM_AUDIO ? "Audio" : "Subtitles");
}

static gchar *
size_text(gint64 bytes)
{
	return axis_web_browser_format_size((guint64)MAX(bytes, 0));
}

/* The address of a device's page: Plex's by the id as it always was,
 * another server's by "<kind>~<id>" (axis_compat_handle()). */
static gchar *
handle_href(const gchar *handle)
{
	g_autofree gchar *escaped = g_uri_escape_string(handle, NULL, FALSE);

	return g_strdup_printf("/compat/devices/%s", escaped);
}

static gchar *
device_href(
	const gchar *kind,
	const gchar *client_id
){
	g_autofree gchar *handle = axis_compat_handle(kind, client_id);

	return handle_href(handle);
}

static const gchar *
server_label(const gchar *kind)
{
	const gchar *label = axis_compat_kind_label(kind);

	return label != NULL ? label : "unknown";
}

static HtmxNode *
tabs(const gchar *current)
{
	static const struct
	{
		const gchar *path;
		const gchar *label;
	} items[] = {
		{ "/compat", "Overview" },
		{ "/compat/devices", "Devices" },
		{ "/compat/verdicts", "Playback verdicts" },
		{ "/compat/failures", "Converted while playing" },
		{ "/compat/mapping", "Media server libraries and paths" },
		{ "/compat/rescans", "Rescan requests" }
	};
	HtmxNav *nav = htmx_nav_new();
	HtmxUl *list = htmx_ul_new();
	guint i;

	htmx_element_set_attribute(HTMX_ELEMENT(nav), "aria-label", "Player compatibility");
	htmx_element_add_class(HTMX_ELEMENT(list), "tabs");
	for (i = 0; i < G_N_ELEMENTS(items); i++) {
		HtmxLi *item = htmx_li_new();

		if (g_strcmp0(current, items[i].path) == 0) {
			HtmxSpan *here = htmx_span_new();

			htmx_element_set_attribute(HTMX_ELEMENT(here), "aria-current", "page");
			htmx_node_set_text_content(HTMX_NODE(here), items[i].label);
			BROWSER_ADD(item, here);
		} else {
			BROWSER_ADD(item, axis_web_browser_link(items[i].path, items[i].label));
		}
		BROWSER_ADD(list, item);
	}
	BROWSER_ADD(nav, list);
	return HTMX_NODE(nav);
}

static HtmxDiv *
page_start(
	const gchar *path,
	const gchar *title
){
	HtmxDiv *content = htmx_div_new();
	HtmxHeading *heading = htmx_h1_new();

	htmx_node_set_text_content(HTMX_NODE(heading), title);
	BROWSER_ADD(content, heading);
	BROWSER_ADD(content, axis_web_page_intro("What the household's players can play directly, read from your Plex "
		"or Jellyfin server, so that a conversion never produces a file one of them cannot play and the space "
		"savings say what would still play everywhere."));
	BROWSER_ADD(content, tabs(path));
	return content;
}

static HtmxNode *
action_form(
	HtmxRequest *request,
	const gchar *action,
	const gchar *label
){
	HtmxNode *form = axis_web_auth_post_form(request, action);

	htmx_element_add_class(HTMX_ELEMENT(form), "inline-form");
	axis_web_auth_add(form, axis_web_auth_button(label, NULL));
	return form;
}

static HtmxResponse *
page_response(
	HtmxRequest *request,
	const gchar *title,
	HtmxDiv     *content
){
	return axis_web_page_respond(request, title, HTMX_NODE(content));
}

static const gchar *
done_message(const gchar *key)
{
	if (g_strcmp0(key, "poll-requested") == 0)
		return "The sessions will be looked at now; reload in a moment.";
	if (g_strcmp0(key, "history-requested") == 0)
		return "The play history will be imported now; reload in a moment.";
	if (g_strcmp0(key, "mapping-added") == 0)
		return "The path was mapped. Rescan requests of the last seven days that no mapping reached, and that the "
			"path map reaches now, are sent again (see Rescan requests).";
	if (g_strcmp0(key, "mapping-removed") == 0)
		return "The mapping was removed.";
	if (g_strcmp0(key, "asked-again") == 0)
		return "The request waits again and is sent within a minute, if a path mapping reaches its folder now.";
	if (g_strcmp0(key, "already-waiting") == 0)
		return "A request for that folder is waiting already; the server is told with that one.";
	if (g_strcmp0(key, "pinned") == 0)
		return "The device is always considered now.";
	if (g_strcmp0(key, "unpinned") == 0)
		return "The device is no longer always considered.";
	return NULL;
}

/* ── Work ───────────────────────────────────────────────────────── */

typedef enum
{
	PAGE_OVERVIEW,
	PAGE_DEVICES,
	PAGE_DEVICE,
	PAGE_VERDICTS,
	PAGE_FAILURES,
	PAGE_MAPPING,
	PAGE_RESCANS,
	ACTION_TEST,
	ACTION_POLL,
	ACTION_HISTORY,
	ACTION_MAP_ADD,
	ACTION_MAP_REMOVE,
	ACTION_PIN,
	ACTION_ASK_AGAIN
} Kind;

typedef struct
{
	Kind                  kind;
	gchar                *id;		/* a device, or the text of a number */
	gchar                *server_path;
	gchar                *axis_path;
	gboolean              flag;
	gchar                *user;
	gchar                *server;	/* a kind of server, for the test and the path map */
	GPtrArray            *addresses;	/* the address of each kind (utf8 or NULL), in the kinds' order */
	AxisCompatReport     *report;
	GPtrArray            *devices;
	AxisCompatDeviceInfo *device;
	GPtrArray            *failures;
	GPtrArray            *files;
	GPtrArray            *map;
	GPtrArray            *notifications;	/* AxisCompatNotifyInfo */
	GPtrArray            *roots;		/* AxisCompatNotifyRoot */
	AxisCompatTestResult  test;
	gchar                *problem;
	gchar                *address;
	gint64                job_id;
	AxisCompatAskAgain    asked;
	GError               *error;
} Work;

static void
work_free(gpointer data)
{
	Work *work = data;

	g_free(work->id);
	g_free(work->server_path);
	g_free(work->axis_path);
	g_free(work->user);
	g_free(work->server);
	g_clear_pointer(&work->addresses, g_ptr_array_unref);
	g_free(work->problem);
	g_free(work->address);
	axis_compat_report_free(work->report);
	g_clear_pointer(&work->devices, g_ptr_array_unref);
	axis_compat_device_info_free(work->device);
	g_clear_pointer(&work->failures, g_ptr_array_unref);
	g_clear_pointer(&work->files, g_ptr_array_unref);
	g_clear_pointer(&work->map, g_ptr_array_unref);
	g_clear_pointer(&work->notifications, g_ptr_array_unref);
	g_clear_pointer(&work->roots, g_ptr_array_unref);
	axis_compat_test_result_clear(&work->test);
	g_clear_error(&work->error);
	g_free(work);
}

static void
work_run(
	AxisWebCompat *self,
	gpointer       data
){
	Work *work = data;
	AxisCompat *compat = self->compat;

	switch (work->kind) {
	case PAGE_OVERVIEW: {
		guint i;

		work->address = axis_compat_describe_address(compat);
		work->addresses = g_ptr_array_new_with_free_func(g_free);
		for (i = 0; i < axis_compat_n_kinds(); i++)
			g_ptr_array_add(work->addresses, axis_compat_describe_address_kind(compat, axis_compat_kind_name(i)));
		axis_compat_compute_report(compat, &work->report, &work->error);
		break;
	}
	case PAGE_DEVICES:
		axis_compat_list_devices(compat, &work->devices, &work->error);
		break;
	case PAGE_DEVICE:
		axis_compat_get_device(compat, work->id, &work->device, &work->error);
		break;
	case PAGE_VERDICTS:
		axis_compat_compute_report(compat, &work->report, &work->error);
		break;
	case PAGE_FAILURES:
		axis_compat_report_failures(compat, &work->failures, &work->files, &work->error);
		break;
	case PAGE_MAPPING:
		if (axis_compat_compute_report(compat, &work->report, &work->error))
			axis_compat_list_path_map(compat, &work->map, &work->error);
		break;
	case PAGE_RESCANS:
		if (axis_compat_notify_roots(compat, &work->roots, &work->error))
			axis_compat_notify_list(compat, 100, &work->notifications, &work->error);
		break;
	case ACTION_TEST:
		axis_compat_test_connection_kind(compat, work->server != NULL ? work->server : AXIS_COMPAT_KIND_PLEX, NULL,
			&work->test);
		break;
	case ACTION_POLL:
		axis_compat_request_poll(compat, &work->job_id, &work->error);
		break;
	case ACTION_HISTORY:
		axis_compat_request_history(compat, &work->job_id, &work->error);
		break;
	case ACTION_MAP_ADD:
		axis_compat_path_map_add_kind(compat, work->server != NULL ? work->server : AXIS_COMPAT_KIND_PLEX,
			work->server_path, work->axis_path, &work->problem, &work->error);
		break;
	case ACTION_MAP_REMOVE:
		axis_compat_path_map_remove(compat, g_ascii_strtoll(work->id, NULL, 10), &work->error);
		break;
	case ACTION_PIN:
		axis_compat_set_pinned(compat, work->id, work->flag, &work->error);
		break;
	case ACTION_ASK_AGAIN:
		axis_compat_notify_ask_again(compat, g_ascii_strtoll(work->id, NULL, 10), &work->asked, &work->error);
		break;
	}
}

/* ── Overview ───────────────────────────────────────────────────── */

static gchar *
coverage_text(const AxisCompatCoverage *c)
{
	GString *out = g_string_new(NULL);
	g_autofree gchar *since = NULL;

	if (c->started_us == 0) {
		g_string_append(out, "Watching has not started: it starts when the connection is first used. ");
	} else if (c->sessions == 0) {
		since = axis_web_storage_format_time(c->started_us, "never");
		g_string_append_printf(out, "Sessions have been watched since %s, and none has been seen playing yet. ",
			since);
	} else {
		g_autofree gchar *from = axis_web_storage_format_time(c->first_session_us, "never");
		g_autofree gchar *to = axis_web_storage_format_time(c->last_session_us, "never");
		gdouble days = (gdouble)(c->last_session_us - c->first_session_us) / (86400.0 * 1000000.0);

		g_string_append_printf(out, "%" G_GINT64_FORMAT " session%s observed from %s to %s (%.1f days), from %"
			G_GINT64_FORMAT " device%s: %" G_GINT64_FORMAT " on the local network, %" G_GINT64_FORMAT
			" remote, %" G_GINT64_FORMAT " matched to a file in the index. ",
			c->sessions, c->sessions == 1 ? "" : "s", from, to, days, c->observed_devices,
			c->observed_devices == 1 ? "" : "s", c->local_sessions, c->remote_sessions, c->matched_sessions);
	}
	if (c->plays == 0) {
		g_string_append(out, "No past plays have been imported. ");
	} else {
		g_autofree gchar *from = axis_web_storage_format_time(c->first_play_us, "never");
		g_autofree gchar *to = axis_web_storage_format_time(c->last_play_us, "never");

		g_string_append_printf(out, "%" G_GINT64_FORMAT " past play%s imported, from %s to %s; they say which "
			"devices play which library, not how. ", c->plays, c->plays == 1 ? "" : "s", from, to);
	}
	return g_string_free(out, FALSE);
}

/* A button that posts the kind of server it concerns (Plex's, the first,
 * posts nothing: it always was the one). */
static HtmxNode *
kind_action_form(
	HtmxRequest *request,
	const gchar *action,
	const gchar *label,
	const gchar *kind
){
	HtmxNode *form = axis_web_auth_post_form(request, action);

	htmx_element_add_class(HTMX_ELEMENT(form), "inline-form");
	if (g_strcmp0(kind, AXIS_COMPAT_KIND_PLEX) != 0)
		axis_web_auth_add(form, axis_web_auth_hidden("server", kind));
	axis_web_auth_add(form, axis_web_auth_button(label, NULL));
	return form;
}

#define CONNECTION_PLACE "compat-connection"

/* The card of one server: who it is, how the looks go, and what it can
 * and cannot tell. */
static HtmxNode *
connection_card(
	HtmxRequest                  *request,
	const AxisCompatServerStatus *c,
	const gchar                  *address,
	const gchar                  *title,
	gboolean                      first,
	AxisWebNotice                *notice
){
	HtmxNode *card = axis_web_storage_card(title);
	HtmxNode *facts = axis_web_browser_facts_new();
	g_autofree gchar *name = NULL;
	g_autofree gchar *poll = NULL;
	g_autofree gchar *ok = NULL;
	g_autofree gchar *next = NULL;
	g_autofree gchar *history = NULL;
	HtmxDiv *buttons = htmx_div_new();

	/* The first card holds the buttons that look now and import. */
	if (first) {
		htmx_element_set_id(HTMX_ELEMENT(card), CONNECTION_PLACE);
		axis_web_notice_put(notice, card, CONNECTION_PLACE);
	}
	if (!c->configured) {
		BROWSER_ADD(card, axis_web_auth_notice(FALSE, "The media server connection is not set up. Enter its address "
			"and token under Settings (Player compatibility); until then nothing is contacted and every verdict is "
			"unknown."));
		BROWSER_ADD(card, axis_web_browser_link("/admin/settings", "Settings"));
	}
	if (c->server_name != NULL || c->server_version != NULL)
		name = g_strdup_printf("%s%s%s", c->server_name != NULL ? c->server_name : axis_compat_kind_product(c->kind),
			c->server_version != NULL ? ", version " : "", c->server_version != NULL ? c->server_version : "");
	else
		name = g_strdup("not contacted yet");
	poll = axis_web_storage_format_time(c->last_poll_us, "never");
	ok = axis_web_storage_format_time(c->last_ok_us, "never");
	next = axis_web_storage_format_time(c->next_poll_us, "not scheduled");
	history = c->has_history ? axis_web_storage_format_time(c->history_us, "not yet") :
		g_strdup("not available from this server");
	axis_web_browser_fact(facts, "Media server", name);
	axis_web_browser_fact(facts, "Address", address != NULL ? address : "not set");
	axis_web_browser_fact(facts, "Last look at the sessions", poll);
	axis_web_browser_fact(facts, "Last successful look", ok);
	axis_web_browser_fact(facts, "Next look", next);
	{
		g_autofree gchar *playing = g_strdup_printf("%d", c->playing_now);

		axis_web_browser_fact(facts, "Sessions playing at the last look", playing);
	}
	axis_web_browser_fact(facts, "Play history imported", history);
	BROWSER_ADD(card, facts);
	if (!c->has_history) {
		g_autofree gchar *text = g_strdup_printf("%s keeps no play history per device, so there is none to import. "
			"A device counts toward a library's verdict only once it has been seen playing that library here "
			"(or when it is marked always consider); until then its verdict is unknown, never safe.", c->label);

		BROWSER_ADD(card, axis_web_storage_text("help", text));
	}
	if (c->last_error != NULL) {
		g_autofree gchar *text = g_strdup_printf("The last look failed (%d in a row): %s", c->failures, c->last_error);

		BROWSER_ADD(card, axis_web_auth_notice(TRUE, text));
	}
	if (c->history_error != NULL) {
		g_autofree gchar *text = g_strdup_printf("The last history import failed: %s", c->history_error);

		BROWSER_ADD(card, axis_web_auth_notice(TRUE, text));
	}
	htmx_element_add_class(HTMX_ELEMENT(buttons), "actions");
	if (g_strcmp0(c->kind, AXIS_COMPAT_KIND_PLEX) == 0) {
		BROWSER_ADD(buttons, kind_action_form(request, "/compat/test", "Test the Plex connection", c->kind));
	} else {
		g_autofree gchar *label = g_strdup_printf("Test the %s connection", c->label);

		BROWSER_ADD(buttons, kind_action_form(request, "/compat/test", label, c->kind));
	}
	if (first)
		BROWSER_ADD(buttons, action_form(request, "/compat/poll", "Check what the media servers are playing now"));
	if (c->has_history)
		BROWSER_ADD(buttons, action_form(request, "/compat/history", "Import the play history now"));
	BROWSER_ADD(card, buttons);
	return card;
}

static HtmxNode *
glance_card(const AxisCompatReport *report)
{
	static const AxisWebColumn columns[] = {
		{ "Conversion profile", AXIS_WEB_COL_NAME },
		{ "Libraries safe", AXIS_WEB_COL_COUNT },
		{ "Libraries risky", AXIS_WEB_COL_COUNT },
		{ "Libraries unknown", AXIS_WEB_COL_COUNT },
		{ "Libraries judged", AXIS_WEB_COL_COUNT },
		{ NULL, 0 }
	};
	/* Every profile the report judged (built-ins first, as it has them). */
	g_autoptr(GArray) targets = g_array_new(FALSE, TRUE, sizeof(AxisEstimateTarget));
	HtmxNode *card = axis_web_storage_card("Playback verdicts at a glance");
	HtmxNode *table;
	HtmxTbody *body;
	guint mapped = 0;
	guint i;

	for (i = 0; i < report->libraries->len; i++) {
		const AxisCompatLibraryReport *l = g_ptr_array_index(report->libraries, i);
		guint t;

		if (l->state == AXIS_MAP_MAPPED)
			mapped++;
		for (t = 0; l->targets != NULL && t < l->targets->len; t++) {
			const AxisCompatTargetVerdict *tv = g_ptr_array_index(l->targets, t);
			gboolean seen = FALSE;
			guint k;

			for (k = 0; k < targets->len; k++) {
				if (strcmp(g_array_index(targets, AxisEstimateTarget, k).id, tv->target.id) == 0)
					seen = TRUE;
			}
			if (!seen)
				g_array_append_val(targets, tv->target);
		}
	}
	BROWSER_ADD(card, axis_web_storage_text("help", "A conversion profile is safe for a media server library only "
		"when every device that played it has been seen playing that combination directly. A device never seen "
		"with it makes the verdict unknown, and unknown is never safe: it blocks a recommendation as risky does."));
	if (mapped == 0) {
		BROWSER_ADD(card, axis_web_storage_text(NULL, report->libraries->len == 0 ?
			"The media server has listed no library yet. Test the connection on the Overview tab." :
			"No media server library is mapped to a library folder yet, so there are no verdicts. Map them under "
			"Media server libraries and paths."));
		if (report->libraries->len > 0)
			BROWSER_ADD(card, axis_web_browser_link("/compat/mapping", "Map media server libraries to library folders"));
		return card;
	}
	table = axis_web_table_new("verdict-counts", columns);
	body = htmx_tbody_new();
	for (i = 0; i < targets->len; i++) {
		const AxisEstimateTarget *target = &g_array_index(targets, AxisEstimateTarget, i);
		guint safe;
		guint risky;
		guint unknown;
		g_autofree gchar *name = axis_web_analysis_profile_label(target->family_name, target->name);
		g_autofree gchar *link_text = NULL;
		g_autofree gchar *s = NULL;
		g_autofree gchar *r = NULL;
		g_autofree gchar *u = NULL;
		g_autofree gchar *n = NULL;
		HtmxTr *row;
		HtmxTd *cell;

		axis_compat_report_target_counts(report, target->id, &safe, &risky, &unknown);
		if (safe + risky + unknown == 0)
			continue;
		row = htmx_tr_new();
		cell = htmx_td_new();
		s = g_strdup_printf("%u", safe);
		r = g_strdup_printf("%u", risky);
		u = g_strdup_printf("%u", unknown);
		n = g_strdup_printf("%u", safe + risky + unknown);
		link_text = g_strdup_printf("%s, verdict by library", name);
		BROWSER_ADD(cell, axis_web_browser_link("/compat/verdicts", link_text));
		BROWSER_ADD(row, cell);
		BROWSER_ADD(row, text_cell("num", s));
		BROWSER_ADD(row, text_cell("num", r));
		BROWSER_ADD(row, text_cell("num", u));
		BROWSER_ADD(row, text_cell("num", n));
		BROWSER_ADD(body, row);
	}
	BROWSER_ADD(table, body);
	BROWSER_ADD(card, table);
	return card;
}

static HtmxResponse *
overview_done(
	AxisWebCompat *self,
	HtmxRequest   *request,
	Work          *work
){
	g_autoptr(HtmxDiv) content = page_start("/compat", "Player compatibility");
	const gchar *done = done_message(htmx_request_get_query_param(request, "done"));
	AxisWebNotice notice;
	HtmxNode *card;
	g_autofree gchar *text = NULL;

	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	axis_web_notice_begin(&notice, request, FALSE, g_strdup(done), HTMX_NODE(content));
	BROWSER_ADD(content, axis_web_storage_text(NULL, "Originals are deleted after an Axis conversion, so Axis calls "
		"a conversion profile safe for a media server library only when the devices that play it have been seen "
		"playing it. Everything here comes from the media server, read only: the devices, the sessions observed while they play, and the play history."));
	{
		const AxisCompatCoverage *cov = &work->report->coverage;
		gboolean others = FALSE;
		guint shown = 0;
		guint k;

		for (k = 0; k < cov->servers->len; k++) {
			const AxisCompatServerStatus *s = g_ptr_array_index(cov->servers, k);

			others = others || (s->configured && g_strcmp0(s->kind, AXIS_COMPAT_KIND_PLEX) != 0);
		}
		/* Plex's card is always there, as it always was, unless another
		 * server is the one that is set up. */
		for (k = 0; k < cov->servers->len; k++) {
			const AxisCompatServerStatus *s = g_ptr_array_index(cov->servers, k);
			g_autofree gchar *connection_title = g_strdup_printf("Connection to %s", s->label);

			if (!s->configured && !(g_strcmp0(s->kind, AXIS_COMPAT_KIND_PLEX) == 0 && !others))
				continue;
			BROWSER_ADD(content, connection_card(request, s,
				k < work->addresses->len ? g_ptr_array_index(work->addresses, k) : NULL,
				connection_title, shown == 0, &notice));
			shown++;
		}
	}
	card = axis_web_storage_card("What the evidence covers");
	text = coverage_text(&work->report->coverage);
	BROWSER_ADD(card, axis_web_storage_text(NULL, text));
	BROWSER_ADD(card, axis_web_storage_text("help", "Evidence of direct play, or of the server converting while "
		"playing, can only come from sessions watched while they play, from the day the connection was first used: "
		"the media server's history holds no playback decision. Until a device has been seen playing, every verdict "
		"about it is unknown."));
	BROWSER_ADD(content, card);
	BROWSER_ADD(content, glance_card(work->report));
	axis_web_notice_end(&notice);
	return page_response(request, "Player compatibility", content);
}

/* ── Devices ────────────────────────────────────────────────────── */

static HtmxResponse *
devices_done(
	AxisWebCompat *self,
	HtmxRequest   *request,
	Work          *work
){
	static const AxisWebColumn columns[] = {
		{ "Device", AXIS_WEB_COL_NAME },
		{ "Media server", AXIS_WEB_COL_TEXT },
		{ "Platform", AXIS_WEB_COL_TEXT },
		{ "Sessions seen", AXIS_WEB_COL_COUNT },
		{ "Remote sessions", AXIS_WEB_COL_COUNT },
		{ "Plays that count", AXIS_WEB_COL_COUNT },
		{ "Last active", AXIS_WEB_COL_DATE },
		{ "Always consider", AXIS_WEB_COL_STATE },
		{ NULL, 0 }
	};
	g_autoptr(HtmxDiv) content = page_start("/compat/devices", "Player compatibility: devices");
	HtmxNode *card = axis_web_storage_card("Devices the media servers have seen");
	HtmxNode *table;
	HtmxTbody *body;
	guint i;

	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	BROWSER_ADD(card, axis_web_storage_text("help", "Device names come from the media server and are shown to "
		"administrators only. \"Sessions seen\" counts plays watched while they played: the only source of evidence "
		"about what a device can play. A device marked always consider counts toward every library's verdicts, "
		"however long ago it played."));
	if (work->devices->len == 0) {
		BROWSER_ADD(card, axis_web_storage_text(NULL, "No device has been seen yet: devices appear here once "
			"they play something while a media server is connected (see the Overview tab)."));
		BROWSER_ADD(content, card);
		return page_response(request, "Player compatibility: devices", content);
	}
	table = axis_web_table_new("devices", columns);
	body = htmx_tbody_new();
	for (i = 0; i < work->devices->len; i++) {
		const AxisCompatDeviceInfo *d = g_ptr_array_index(work->devices, i);
		g_autofree gchar *href = device_href(d->server_kind, d->client_id);
		g_autofree gchar *sessions = g_strdup_printf("%u", d->sessions);
		g_autofree gchar *remote = g_strdup_printf("%u", d->remote_sessions);
		g_autofree gchar *plays = g_strdup_printf("%u", d->plays_in_window);
		g_autofree gchar *last = axis_web_storage_format_time(d->last_play_us, "never");
		HtmxTr *row = htmx_tr_new();
		HtmxTd *cell = htmx_td_new();

		BROWSER_ADD(cell, axis_web_browser_link(href, device_label(d->name, d->client_id)));
		BROWSER_ADD(row, cell);
		BROWSER_ADD(row, text_cell(NULL, server_label(d->server_kind)));
		BROWSER_ADD(row, name_cell(d->platform));
		BROWSER_ADD(row, text_cell("num", sessions));
		BROWSER_ADD(row, text_cell("num", remote));
		BROWSER_ADD(row, text_cell("num", plays));
		BROWSER_ADD(row, text_cell(NULL, last));
		BROWSER_ADD(row, text_cell(NULL, d->pinned ? "Yes" : ""));
		BROWSER_ADD(body, row);
	}
	BROWSER_ADD(table, body);
	BROWSER_ADD(card, table);
	BROWSER_ADD(content, card);
	return page_response(request, "Player compatibility: devices", content);
}

static HtmxResponse *
device_done(
	AxisWebCompat *self,
	HtmxRequest   *request,
	Work          *work
){
	static const AxisWebColumn columns[] = {
		{ "Stream", AXIS_WEB_COL_TEXT },
		{ "Combination", AXIS_WEB_COL_NAME },
		{ "What was seen", AXIS_WEB_COL_WIDE },
		{ "Reason the server gave", AXIS_WEB_COL_WIDE },
		{ "Plays", AXIS_WEB_COL_COUNT },
		{ "Remote plays", AXIS_WEB_COL_COUNT },
		{ "Last seen", AXIS_WEB_COL_DATE },
		{ NULL, 0 }
	};
	g_autoptr(HtmxDiv) content = NULL;
	g_autofree gchar *title = NULL;
	const AxisCompatDeviceInfo *d = work->device;
	AxisWebNotice notice;
	HtmxNode *card;
	HtmxNode *facts;
	HtmxNode *form;
	HtmxNode *table;
	HtmxTbody *body;
	g_autofree gchar *action = NULL;
	guint i;

	if (work->error != NULL) {
		if (g_error_matches(work->error, AXIS_COMPAT_ERROR, AXIS_COMPAT_ERROR_NOT_FOUND))
			return not_found(request);
		return axis_web_internal_error(request, work->error);
	}
	title = g_strdup_printf("Player compatibility: %s", device_label(d->name, d->client_id));
	content = page_start("/compat/devices", title);
	axis_web_notice_begin(&notice, request, FALSE,
		g_strdup(done_message(htmx_request_get_query_param(request, "done"))), HTMX_NODE(content));
	card = axis_web_storage_card("About this device");
	htmx_element_set_id(HTMX_ELEMENT(card), "device");
	axis_web_notice_put(&notice, card, "device");
	facts = axis_web_browser_facts_new();
	axis_web_browser_fact_node(facts, "Name", bdi_text(d->name != NULL ? d->name : "(unnamed)"));
	axis_web_browser_fact(facts, "Media server", server_label(d->server_kind));
	axis_web_browser_fact_node(facts, "Platform", bdi_text(d->platform != NULL ? d->platform : "unknown"));
	axis_web_browser_fact_node(facts, "Product", bdi_text(d->product != NULL ? d->product : "unknown"));
	axis_web_browser_fact_node(facts, "Identifier", bdi_text(d->client_id));
	{
		g_autofree gchar *sessions = g_strdup_printf("%u (%u remote)", d->sessions, d->remote_sessions);
		g_autofree gchar *first = axis_web_storage_format_time(d->first_seen_us, "never");

		axis_web_browser_fact(facts, "Sessions seen", sessions);
		axis_web_browser_fact(facts, "First seen", first);
	}
	BROWSER_ADD(card, facts);
	{
		g_autofree gchar *href = device_href(d->server_kind, d->client_id);

		action = g_strdup_printf("%s/pin", href);
	}
	form = axis_web_auth_post_form(request, action);
	htmx_element_add_class(HTMX_ELEMENT(form), "inline-form");
	axis_web_auth_add(form, axis_web_auth_hidden("pinned", d->pinned ? "no" : "yes"));
	axis_web_auth_add(form, axis_web_auth_button(d->pinned ? "Stop always considering this device" :
		"Always consider this device", NULL));
	BROWSER_ADD(card, axis_web_storage_text("help", d->pinned ?
		"This device counts toward every library's verdicts, however long ago it played." :
		"Mark a device that matters but plays rarely (a relative's old TV) so that it counts toward every "
		"library's verdicts, whatever the look-back window."));
	BROWSER_ADD(card, form);
	BROWSER_ADD(content, card);

	card = axis_web_storage_card("What this device has been seen playing");
	BROWSER_ADD(card, axis_web_storage_text("help", "Built only from sessions watched while they played. A "
		"combination not listed here has never been seen on this device: its verdict is unknown, not safe. "
		"Passed through means the codec was copied into other packaging, which shows the codec and says nothing "
		"of the container. Bandwidth limits and conversions of unclear cause are not held against a codec. Where "
		"the server states why it converted a stream (Jellyfin does; Plex does not), its own words are shown, and "
		"a conversion it blames on the container, a profile, a bit depth, a level or a subtitle format is held "
		"only against that, never against the codec elsewhere."));
	if (d->rows->len == 0) {
		BROWSER_ADD(card, axis_web_storage_text(NULL, "Nothing has been observed yet."));
		BROWSER_ADD(content, card);
		axis_web_notice_end(&notice);
		return page_response(request, title, content);
	}
	table = axis_web_table_new("device-observations", columns);
	body = htmx_tbody_new();
	for (i = 0; i < d->rows->len; i++) {
		const AxisCompatObsRow *r = g_ptr_array_index(d->rows, i);
		g_autofree gchar *what = axis_compat_describe_row(r);
		g_autofree gchar *effect = r->out_codec != NULL ?
			g_strdup_printf("%s (to %s)", axis_compat_effect_label(r->obs.effect), r->out_codec) :
			g_strdup(axis_compat_effect_label(r->obs.effect));
		/* A conversion the server blames on a property is that, not the codec. */
		g_autofree gchar *blamed = r->obs.property != AXIS_PROPERTY_NONE ?
			g_strdup_printf("converted: the device did not take %s", axis_compat_property_label(r->obs.property)) :
			g_strdup(effect);
		g_autofree gchar *plays = g_strdup_printf("%u", r->obs.plays);
		g_autofree gchar *remote = g_strdup_printf("%u", r->obs.remote_plays);
		g_autofree gchar *last = axis_web_storage_format_time(r->last_us, "never");
		HtmxTr *row = htmx_tr_new();

		BROWSER_ADD(row, text_cell(NULL, kind_label(r->obs.combo.kind)));
		BROWSER_ADD(row, name_cell(what));
		BROWSER_ADD(row, text_cell(NULL, blamed));
		BROWSER_ADD(row, name_cell(r->stated != NULL ? r->stated : ""));
		BROWSER_ADD(row, text_cell("num", plays));
		BROWSER_ADD(row, text_cell("num", remote));
		BROWSER_ADD(row, text_cell(NULL, last));
		BROWSER_ADD(body, row);
	}
	BROWSER_ADD(table, body);
	BROWSER_ADD(card, table);
	BROWSER_ADD(content, card);
	axis_web_notice_end(&notice);
	return page_response(request, title, content);
}

/* ── Verdicts ───────────────────────────────────────────────────── */

static HtmxNode *
target_devices_table(const AxisCompatTargetVerdict *tv)
{
	static const AxisWebColumn columns[] = {
		{ "Device", AXIS_WEB_COL_NAME },
		{ "Media server", AXIS_WEB_COL_TEXT },
		{ "Playback verdict", AXIS_WEB_COL_STATE },
		{ "Why", AXIS_WEB_COL_WIDE },
		{ "Plays that count", AXIS_WEB_COL_COUNT },
		{ NULL, 0 }
	};
	HtmxNode *table = axis_web_table_new("device-verdicts", columns);
	HtmxTbody *body = htmx_tbody_new();
	guint i;

	for (i = 0; i < tv->devices->len; i++) {
		const AxisCompatDeviceVerdict *d = g_ptr_array_index(tv->devices, i);
		g_autofree gchar *plays = g_strdup_printf("%u", d->plays);
		g_autofree gchar *why = g_strdup_printf("%s%s", d->detail != NULL ? d->detail : "",
			d->pinned ? " (always considered)" : "");
		HtmxTr *row = htmx_tr_new();
		HtmxTd *cell = htmx_td_new();
		HtmxTd *verdict = htmx_td_new();
		g_autofree gchar *href = device_href(d->server_kind, d->client_id);

		BROWSER_ADD(cell, axis_web_browser_link(href, device_label(d->name, d->client_id)));
		BROWSER_ADD(row, cell);
		BROWSER_ADD(row, text_cell(NULL, server_label(d->server_kind)));
		BROWSER_ADD(verdict, verdict_badge(d->verdict));
		BROWSER_ADD(row, verdict);
		BROWSER_ADD(row, name_cell(why));
		BROWSER_ADD(row, text_cell("num", plays));
		BROWSER_ADD(body, row);
	}
	BROWSER_ADD(table, body);
	return table;
}

static HtmxNode *
library_card(const AxisCompatLibraryReport *l)
{
	g_autofree gchar *card_title = g_strdup_printf("%s (%s library)", l->title != NULL ? l->title : l->key,
		server_label(l->server_kind));
	HtmxNode *card = axis_web_storage_card(card_title);
	g_autofree gchar *roots = NULL;
	g_autofree gchar *line = NULL;
	guint i;

	{
		GString *names = g_string_new(NULL);

		for (i = 0; i < l->root_names->len; i++)
			g_string_append_printf(names, "%s%s", i > 0 ? ", " : "", (const gchar *)g_ptr_array_index(l->root_names, i));
		roots = g_string_free(names, FALSE);
	}
	{
		gboolean video = FALSE;

		for (i = 0; i < l->targets->len; i++)
			video = video || ((AxisCompatTargetVerdict *)g_ptr_array_index(l->targets, i))->target.media ==
				AXIS_TARGET_VIDEO;
		line = g_strdup_printf("Maps to: %s. %u device%s considered (played it within the look-back window, or "
			"always considered).%s%s%s", roots, l->n_devices, l->n_devices == 1 ? "" : "s",
			video ? " Highest resolution in the library: " : "", video ? l->res_class : "",
			video ? (l->probed ? "." : " (assumed: not every file has been probed).") : "");
	}
	BROWSER_ADD(card, axis_web_storage_text("help", line));
	if (l->n_devices == 0)
		BROWSER_ADD(card, axis_web_auth_notice(FALSE, "No play of this library falls in the period that counts, so "
			"the verdict for every conversion profile is unknown."));
	for (i = 0; i < l->targets->len; i++) {
		const AxisCompatTargetVerdict *tv = g_ptr_array_index(l->targets, i);
		HtmxDiv *block = htmx_div_new();
		HtmxHeading *h3 = htmx_h3_new();
		HtmxDetails *details = htmx_details_new();
		g_autofree gchar *name = axis_web_analysis_profile_label(tv->target.family_name, tv->target.name);
		g_autofree gchar *saving = tv->saving >= 0 ? size_text(tv->saving) : NULL;
		g_autofree gchar *summary = g_strdup_printf("%s%s: %u %s safe, %u risky, %u unknown", name,
			(gint)i == l->recommended ? " (recommended)" : "", tv->n_safe, tv->n_safe == 1 ? "device" : "devices",
			tv->n_risky, tv->n_unknown);

		htmx_node_set_text_content(HTMX_NODE(h3), name);
		BROWSER_ADD(block, h3);
		BROWSER_ADD(block, verdict_badge(tv->verdict));
		if (saving != NULL) {
			g_autofree gchar *text = g_strdup_printf("Estimated saving in this library: about %s.", saving);

			BROWSER_ADD(block, axis_web_storage_text("help", text));
		} else {
			BROWSER_ADD(block, axis_web_storage_text("help", "No saving has been estimated yet."));
				BROWSER_ADD(block, axis_web_browser_link("/savings", "See Space savings"));
		}
		BROWSER_ADD(details, htmx_summary_new_with_text(summary));
		if (tv->devices->len > 0)
			BROWSER_ADD(details, target_devices_table(tv));
		BROWSER_ADD(block, details);
		BROWSER_ADD(card, block);
	}
	if (l->targets->len == 0) {
		BROWSER_ADD(card, axis_web_storage_text(NULL, "No conversion profile applies to the library folders this "
			"media server library maps to."));
	} else if (l->recommended >= 0) {
		const AxisCompatTargetVerdict *tv = g_ptr_array_index(l->targets, (guint)l->recommended);
		g_autofree gchar *rec_name = axis_web_analysis_profile_label(tv->target.family_name, tv->target.name);
		g_autofree gchar *saving = size_text(tv->saving);
		g_autofree gchar *text = g_strdup_printf("Recommended for this library: %s, saving about %s. Every "
			"device that plays it has been seen playing that combination directly.", rec_name, saving);

		BROWSER_ADD(card, axis_web_auth_notice(FALSE, text));
	} else if (l->best >= 0) {
		const AxisCompatTargetVerdict *tv = g_ptr_array_index(l->targets, (guint)l->best);
		g_autofree gchar *best_name = axis_web_analysis_profile_label(tv->target.family_name, tv->target.name);
		g_autofree gchar *saving = size_text(tv->saving);
		GString *blockers = g_string_new(NULL);
		g_autofree gchar *text = NULL;
		guint shown = 0;

		for (i = 0; i < tv->devices->len; i++) {
			const AxisCompatDeviceVerdict *d = g_ptr_array_index(tv->devices, i);

			if (d->verdict == AXIS_VERDICT_SAFE)
				continue;
			g_string_append_printf(blockers, "%s%s (%s)", shown > 0 ? ", " : "", device_label(d->name, d->client_id),
				d->verdict == AXIS_VERDICT_RISKY ? "risky" : "unknown");
			shown++;
		}
		if (shown == 0)
			g_string_append(blockers, "no device has played this library, so nothing is known");
		text = g_strdup_printf("No conversion profile is safe for this media server library. The one that would save "
			"most, %s (about %s), is held back by: %s.", best_name, saving, blockers->str);
		g_string_free(blockers, TRUE);
		BROWSER_ADD(card, axis_web_auth_notice(TRUE, text));
		if (l->next >= 0) {
			const AxisCompatTargetVerdict *nx = g_ptr_array_index(l->targets, (guint)l->next);
			g_autofree gchar *next_saving = size_text(nx->saving);
			g_autofree gchar *next_name = axis_web_analysis_profile_label(nx->target.family_name, nx->target.name);
			g_autofree gchar *next_text = g_strdup_printf("The next largest saving: %s, about %s (%s).",
				next_name, next_saving, axis_compat_verdict_label(nx->verdict));

			BROWSER_ADD(card, axis_web_storage_text("help", next_text));
		}
	} else {
		BROWSER_ADD(card, axis_web_storage_text("help", "No conversion profile has an estimated saving here, so nothing is "
			"recommended."));
	}
	return card;
}

static HtmxResponse *
verdicts_done(
	AxisWebCompat *self,
	HtmxRequest   *request,
	Work          *work
){
	g_autoptr(HtmxDiv) content = page_start("/compat/verdicts", "Player compatibility: playback verdicts");
	HtmxNode *intro = axis_web_storage_card("How the playback verdicts are decided");
	guint shown = 0;
	guint i;

	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	BROWSER_ADD(intro, axis_web_storage_text(NULL, "For each conversion profile and media server library: safe if every device that played "
		"the library in the look-back window has been seen playing that combination directly (or a harder one of the "
		"same codec and container); risky if one has been seen having the server convert the codec for it; unknown "
		"if any has never been seen with it. Unknown is never safe, and a library nobody has played is unknown."));
	if (work->report->have_estimates) {
		g_autofree gchar *when = axis_web_storage_format_time(work->report->estimates_us, "unknown");
		g_autofree gchar *text = g_strdup_printf("Savings are from the estimates of %s.", when);

		BROWSER_ADD(intro, axis_web_storage_text("help", text));
	}
	BROWSER_ADD(content, intro);
	for (i = 0; i < work->report->libraries->len; i++) {
		const AxisCompatLibraryReport *l = g_ptr_array_index(work->report->libraries, i);

		if (l->state != AXIS_MAP_MAPPED)
			continue;
		BROWSER_ADD(content, library_card(l));
		shown++;
	}
	if (shown == 0) {
		HtmxNode *card = axis_web_storage_card("No playback verdicts yet");

		BROWSER_ADD(card, axis_web_storage_text(NULL, "No media server library is mapped to a library folder yet, "
			"so there is nothing to judge. Libraries that cannot be mapped are left out of the verdicts, not guessed."));
		BROWSER_ADD(card, axis_web_browser_link("/compat/mapping", "Map media server libraries to library folders"));
		BROWSER_ADD(content, card);
	}
	return page_response(request, "Player compatibility: playback verdicts", content);
}

/* ── What fails today ───────────────────────────────────────────── */

static HtmxResponse *
failures_done(
	AxisWebCompat *self,
	HtmxRequest   *request,
	Work          *work
){
	static const AxisWebColumn columns[] = {
		{ "Device", AXIS_WEB_COL_NAME },
		{ "Media server", AXIS_WEB_COL_TEXT },
		{ "Stream", AXIS_WEB_COL_TEXT },
		{ "Codec", AXIS_WEB_COL_TEXT },
		{ "Converted to", AXIS_WEB_COL_TEXT },
		{ "Why", AXIS_WEB_COL_WIDE },
		{ "Plays", AXIS_WEB_COL_COUNT },
		{ "Remote plays", AXIS_WEB_COL_COUNT },
		{ "Files", AXIS_WEB_COL_COUNT },
		{ "Size", AXIS_WEB_COL_SIZE },
		{ NULL, 0 }
	};
	static const AxisWebColumn file_columns[] = {
		{ "File", AXIS_WEB_COL_NAME },
		{ "Size", AXIS_WEB_COL_SIZE },
		{ "Plays", AXIS_WEB_COL_COUNT },
		{ "Devices", AXIS_WEB_COL_COUNT },
		{ NULL, 0 }
	};
	g_autoptr(HtmxDiv) content = page_start("/compat/failures", "Player compatibility: converted while playing");
	HtmxNode *card = axis_web_storage_card("Streams the media server converted while playing");
	HtmxNode *table;
	HtmxTbody *body;
	guint i;

	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	BROWSER_ADD(card, axis_web_storage_text("help", "Streams the media server converted while a device played "
		"them, by device and reason. Conversions explained by a bandwidth or quality limit and those of unclear "
		"cause are listed too but are not held against the device. The bytes are those of the matched files in "
		"the index; sessions that could not be matched to a file count in the plays only. Converting such "
		"files to a format the device takes would fix their playback."));
	if (work->failures->len == 0) {
		BROWSER_ADD(card, axis_web_storage_text(NULL, "No stream has been converted while playing yet."));
	} else {
		table = axis_web_table_new("conversions", columns);
		body = htmx_tbody_new();
		for (i = 0; i < work->failures->len; i++) {
			const AxisCompatFailure *f = g_ptr_array_index(work->failures, i);
			g_autofree gchar *plays = g_strdup_printf("%u", f->plays);
			g_autofree gchar *remote = g_strdup_printf("%u", f->remote_plays);
			g_autofree gchar *files = g_strdup_printf("%u", f->files);
			g_autofree gchar *bytes = size_text(f->bytes);
			HtmxTr *row = htmx_tr_new();
			HtmxTd *cell = htmx_td_new();
			g_autofree gchar *href = device_href(f->server_kind, f->client_id);
			g_autofree gchar *why = NULL;

			if (f->property != AXIS_PROPERTY_NONE)
				why = g_strdup_printf("converted: the device did not take %s%s%s",
					axis_compat_property_label(f->property), f->stated != NULL ? ": " : "",
					f->stated != NULL ? f->stated : "");
			else if (f->stated != NULL)
				why = g_strdup_printf("%s: %s", axis_compat_effect_label(f->effect), f->stated);
			else
				why = g_strdup(axis_compat_effect_label(f->effect));
			BROWSER_ADD(cell, axis_web_browser_link(href, device_label(f->device_name, f->client_id)));
			BROWSER_ADD(row, cell);
			BROWSER_ADD(row, text_cell(NULL, server_label(f->server_kind)));
			BROWSER_ADD(row, text_cell(NULL, kind_label(f->kind)));
			BROWSER_ADD(row, name_cell(f->codec));
			BROWSER_ADD(row, name_cell(f->out_codec[0] != '\0' ? f->out_codec : "(not stated)"));
			BROWSER_ADD(row, name_cell(why));
			BROWSER_ADD(row, text_cell("num", plays));
			BROWSER_ADD(row, text_cell("num", remote));
			BROWSER_ADD(row, text_cell("num", files));
			BROWSER_ADD(row, text_cell("num", bytes));
			BROWSER_ADD(body, row);
		}
		BROWSER_ADD(table, body);
		BROWSER_ADD(card, table);
	}
	BROWSER_ADD(content, card);

	card = axis_web_storage_card("Files the media server converts most often while playing");
	if (work->files->len == 0) {
		BROWSER_ADD(card, axis_web_storage_text(NULL, "No converted play has been matched to a file in the index."));
	} else {
		table = axis_web_table_new("converted-files", file_columns);
		body = htmx_tbody_new();
		for (i = 0; i < work->files->len; i++) {
			const AxisCompatFailedFile *f = g_ptr_array_index(work->files, i);
			g_autofree gchar *href = g_strdup_printf("/library/file/%" G_GINT64_FORMAT, f->file_id);
			g_autofree gchar *size = size_text(f->size);
			g_autofree gchar *plays = g_strdup_printf("%u", f->plays);
			g_autofree gchar *devices = g_strdup_printf("%u", f->devices);
			HtmxTr *row = htmx_tr_new();
			HtmxTd *cell = htmx_td_new();
			g_autofree gchar *shown = axis_library_name_display(f->rel_path);

			BROWSER_ADD(cell, axis_web_browser_link(href, shown));
			BROWSER_ADD(row, cell);
			BROWSER_ADD(row, text_cell("num", size));
			BROWSER_ADD(row, text_cell("num", plays));
			BROWSER_ADD(row, text_cell("num", devices));
			BROWSER_ADD(body, row);
		}
		BROWSER_ADD(table, body);
		BROWSER_ADD(card, table);
	}
	BROWSER_ADD(content, card);
	return page_response(request, "Player compatibility: converted while playing", content);
}

/* ── Libraries and paths ────────────────────────────────────────── */

static HtmxNode *
suggestion_form(
	HtmxRequest                *request,
	const gchar                *kind,
	const AxisCompatSuggestion *s
){
	HtmxNode *form = axis_web_auth_post_form(request, "/compat/mapping/add");
	g_autofree gchar *label = g_strdup_printf("Map this folder to %s", s->root_path);

	htmx_element_add_class(HTMX_ELEMENT(form), "inline-form");
	if (g_strcmp0(kind, AXIS_COMPAT_KIND_PLEX) != 0)
		axis_web_auth_add(form, axis_web_auth_hidden("server", kind));
	axis_web_auth_add(form, axis_web_auth_hidden("server_path", s->folder));
	axis_web_auth_add(form, axis_web_auth_hidden("axis_path", s->root_path));
	axis_web_auth_add(form, axis_web_auth_button(label, NULL));
	return form;
}

static HtmxResponse *
mapping_done(
	AxisWebCompat *self,
	HtmxRequest   *request,
	Work          *work
){
	static const AxisWebColumn columns[] = {
		{ "Media server library", AXIS_WEB_COL_NAME },
		{ "Media server", AXIS_WEB_COL_TEXT },
		{ "Media type (as the server names it)", AXIS_WEB_COL_TEXT },
		{ "Folders (as the server sees them)", AXIS_WEB_COL_NAME },
		{ "Mapped to", AXIS_WEB_COL_WIDE },
		{ NULL, 0 }
	};
	static const AxisWebColumn map_columns[] = {
		{ "Media server", AXIS_WEB_COL_TEXT },
		{ "Media server path", AXIS_WEB_COL_NAME },
		{ "Axis path", AXIS_WEB_COL_NAME },
		{ "Remove", AXIS_WEB_COL_ACTIONS },
		{ NULL, 0 }
	};
	g_autoptr(HtmxDiv) content = page_start("/compat/mapping", "Player compatibility: media server libraries and paths");
	const gchar *done = done_message(htmx_request_get_query_param(request, "done"));
	AxisWebNotice notice;
	HtmxNode *card;
	HtmxNode *table;
	HtmxNode *form;
	HtmxTbody *body;
	guint i;

	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	axis_web_notice_begin(&notice, request, FALSE, g_strdup(done), HTMX_NODE(content));
	if (work->problem != NULL)
		BROWSER_ADD(content, axis_web_auth_notice(TRUE, work->problem));

	card = axis_web_storage_card("The media server's libraries");
	BROWSER_ADD(card, axis_web_storage_text("help", "A library is mapped when every one of its folders, taken "
		"through the path map below, is a library folder, inside one or contains one. A library that is not fully "
		"mapped is excluded from verdicts; nothing is guessed. A suggestion is offered when exactly one library "
		"folder has the most trailing path parts in common with a folder, and is applied only when you press its "
		"button."));
	if (work->report->libraries->len == 0) {
		BROWSER_ADD(card, axis_web_storage_text(NULL, "The media server has listed no library yet. Test the "
			"connection on the Overview tab."));
		BROWSER_ADD(card, axis_web_browser_link("/compat", "Overview of Player compatibility"));
	} else {
		table = axis_web_table_new("server-libraries", columns);
		body = htmx_tbody_new();
		for (i = 0; i < work->report->libraries->len; i++) {
			const AxisCompatLibraryReport *l = g_ptr_array_index(work->report->libraries, i);
			HtmxTr *row = htmx_tr_new();
			HtmxTd *folders = htmx_td_new();
			HtmxTd *mapped = htmx_td_new();
			guint j;

			BROWSER_ADD(row, name_cell(l->title != NULL ? l->title : l->key));
			BROWSER_ADD(row, text_cell(NULL, server_label(l->server_kind)));
			BROWSER_ADD(row, text_cell(NULL, l->kind));
			{
				HtmxUl *list = htmx_ul_new();

				for (j = 0; j < l->folders->len; j++) {
					const gchar *folder = g_ptr_array_index(l->folders, j);
					HtmxLi *item = htmx_li_new();
					gboolean missing = FALSE;
					guint k;

					for (k = 0; k < l->unmapped_folders->len; k++)
						missing = missing || g_strcmp0(g_ptr_array_index(l->unmapped_folders, k), folder) == 0;
					BROWSER_ADD(item, bdi_text(folder));
					if (missing)
						BROWSER_ADD(item, axis_web_browser_badge("Not mapped", "badge-warn"));
					BROWSER_ADD(list, item);
				}
				if (l->folders->len == 0)
					BROWSER_ADD(folders, axis_web_storage_text("help", "The server lists no folder."));
				else
					BROWSER_ADD(folders, list);
			}
			BROWSER_ADD(row, folders);
			if (l->state == AXIS_MAP_MAPPED) {
				GString *names = g_string_new(NULL);

				for (j = 0; j < l->root_names->len; j++)
					g_string_append_printf(names, "%s%s", j > 0 ? ", " : "",
						(const gchar *)g_ptr_array_index(l->root_names, j));
				BROWSER_ADD(mapped, axis_web_browser_badge("Mapped", "badge-succeeded"));
				BROWSER_ADD(mapped, bdi_text(names->str));
				g_string_free(names, TRUE);
			} else {
				BROWSER_ADD(mapped, axis_web_browser_badge(l->state == AXIS_MAP_PARTLY ? "Partly mapped" : "Not mapped",
					"badge-warn"));
				for (j = 0; j < l->suggestions->len; j++)
					BROWSER_ADD(mapped, suggestion_form(request, l->server_kind, g_ptr_array_index(l->suggestions, j)));
			}
			BROWSER_ADD(row, mapped);
			BROWSER_ADD(body, row);
		}
		BROWSER_ADD(table, body);
		BROWSER_ADD(card, table);
	}
	BROWSER_ADD(content, card);

	card = axis_web_storage_card("Path map");
	htmx_element_set_id(HTMX_ELEMENT(card), "path-map");
	axis_web_notice_put(&notice, card, "path-map");
	BROWSER_ADD(card, axis_web_storage_text("help", "The media server sees its libraries under its own paths (for "
		"example /data/movies); Axis sees the same directories under others (for example /media/disk3/movies). "
		"The longest matching prefix wins. Played files are matched to the index through this map."));
	if (work->map->len == 0) {
		BROWSER_ADD(card, axis_web_storage_text(NULL, "The path map is empty: add a mapping with the form below."));
	} else {
		table = axis_web_table_new("path-map", map_columns);
		body = htmx_tbody_new();
		for (i = 0; i < work->map->len; i++) {
			const AxisCompatPathEntry *e = g_ptr_array_index(work->map, i);
			g_autofree gchar *action = g_strdup_printf("/compat/mapping/%" G_GINT64_FORMAT "/remove", e->id);
			HtmxTr *row = htmx_tr_new();
			HtmxTd *cell = htmx_td_new();

			BROWSER_ADD(row, text_cell(NULL, server_label(e->kind != NULL ? e->kind : AXIS_COMPAT_KIND_PLEX)));
			BROWSER_ADD(row, name_cell(e->server));
			BROWSER_ADD(row, name_cell(e->axis));
			BROWSER_ADD(cell, action_form(request, action, "Remove this path mapping"));
			BROWSER_ADD(row, cell);
			BROWSER_ADD(body, row);
		}
		BROWSER_ADD(table, body);
		BROWSER_ADD(card, table);
	}
	form = axis_web_auth_post_form(request, "/compat/mapping/add");
	{
		HtmxDiv *field = htmx_div_new();
		HtmxLabel *label = htmx_label_new_for("server");
		HtmxSelect *select = htmx_select_new_with_name("server");
		guint k;

		htmx_element_set_id(HTMX_ELEMENT(select), "server");
		htmx_element_add_class(HTMX_ELEMENT(field), "field");
		htmx_node_set_text_content(HTMX_NODE(label), "Media server");
		for (k = 0; k < axis_compat_n_kinds(); k++) {
			HtmxOption *option = htmx_option_new_with_value(axis_compat_kind_name(k),
				axis_compat_kind_label(axis_compat_kind_name(k)));

			BROWSER_ADD(select, option);
		}
		BROWSER_ADD(field, label);
		BROWSER_ADD(field, select);
		axis_web_auth_add(form, field);
	}
	axis_web_auth_add(form, axis_web_storage_text_field("server-path", "Media server path", "server_path", NULL,
		"/data/movies", NULL));
	axis_web_auth_add(form, axis_web_storage_text_field("axis-path", "Axis path", "axis_path", NULL,
		"/media/disk3/movies", "An absolute path as Axis sees it: a library folder, or a folder that holds library folders."));
	axis_web_auth_add(form, axis_web_auth_button("Add to the path map", NULL));
	BROWSER_ADD(card, form);
	BROWSER_ADD(content, card);
	axis_web_notice_end(&notice);
	return page_response(request, "Player compatibility: media server libraries and paths", content);
}

/* ── Rescan requests ────────────────────────────────────────────── */

static HtmxNode *
state_cell(HtmxNode *badge)
{
	HtmxTd *cell = htmx_td_new();

	BROWSER_ADD(cell, badge);
	return HTMX_NODE(cell);
}

/* What happened to a folder, in words (the stored value stays "created",
 * "modified" or "deleted"). */
static const gchar *
change_label(const gchar *change)
{
	if (g_strcmp0(change, "created") == 0)
		return "Added";
	if (g_strcmp0(change, "deleted") == 0)
		return "Removed";
	return "Changed";
}

static HtmxNode *
notify_state_badge(const gchar *state)
{
	if (g_strcmp0(state, "sent") == 0)
		return axis_web_browser_badge("Sent", "badge-succeeded");
	if (g_strcmp0(state, "dropped") == 0)
		return axis_web_browser_badge("Dropped", "badge-failed");
	if (g_strcmp0(state, "unmapped") == 0)
		return axis_web_browser_badge("Not sent", "badge-warn");
	if (g_strcmp0(state, "skipped") == 0)
		return axis_web_browser_badge("Skipped", "badge-note");
	return axis_web_browser_badge("Waiting", "badge-running");
}

static HtmxResponse *
rescans_done(
	AxisWebCompat *self,
	HtmxRequest   *request,
	Work          *work
){
	static const AxisWebColumn root_columns[] = {
		{ "Media server", AXIS_WEB_COL_TEXT },
		{ "Library folder", AXIS_WEB_COL_NAME },
		{ "As the server sees it", AXIS_WEB_COL_NAME },
		{ "Media server library", AXIS_WEB_COL_NAME },
		{ "Told of changes", AXIS_WEB_COL_STATE },
		{ NULL, 0 }
	};
	static const AxisWebColumn columns[] = {
		{ "Media server", AXIS_WEB_COL_TEXT },
		{ "Folder", AXIS_WEB_COL_NAME },
		{ "What happened", AXIS_WEB_COL_TEXT },
		{ "Tries", AXIS_WEB_COL_COUNT },
		{ "Last change", AXIS_WEB_COL_DATE },
		{ "Outcome", AXIS_WEB_COL_STATE },
		{ "Note", AXIS_WEB_COL_WIDE },
		{ "Send again", AXIS_WEB_COL_ACTIONS },
		{ NULL, 0 }
	};
	g_autoptr(HtmxDiv) content = page_start("/compat/rescans", "Player compatibility: rescan requests");
	const gchar *done = done_message(htmx_request_get_query_param(request, "done"));
	AxisWebNotice notice;
	HtmxNode *card;
	HtmxNode *table;
	HtmxTbody *body;
	guint i;

	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	axis_web_notice_begin(&notice, request, FALSE, g_strdup(done), HTMX_NODE(content));

	card = axis_web_storage_card("Folders the servers are told about");
	BROWSER_ADD(card, axis_web_storage_text("help", "After an import, an upgrade or a removal Axis asks each media "
		"server that is set up to scan the one folder that changed, never a whole library: Plex through a refresh of "
		"the library section that holds the folder, Jellyfin through its \"media updated\" request, the one thing "
		"Axis writes to a media server. The folder is sent under the server's own names, through the path map. A "
		"library folder the path map does not reach, or that no Plex library holds, is not sent anything, and the server finds its "
		"files when it scans for itself. Telling a server can be switched off in Settings (Player compatibility)."));
	if (work->roots->len == 0) {
		BROWSER_ADD(card, axis_web_storage_text(NULL, "No media server is set up, or no library folder holds movies "
			"or TV, so nothing is sent. Set up a media server under Settings, Player compatibility."));
		BROWSER_ADD(card, axis_web_browser_link("/admin/settings", "Settings"));
	} else {
		table = axis_web_table_new("rescan-roots", root_columns);
		body = htmx_tbody_new();
		for (i = 0; i < work->roots->len; i++) {
			const AxisCompatNotifyRoot *r = g_ptr_array_index(work->roots, i);
			g_autofree gchar *words = NULL;
			HtmxTr *row = htmx_tr_new();

			BROWSER_ADD(row, text_cell(NULL, server_label(r->server_kind)));
			{
				g_autofree gchar *text = g_strdup_printf("%s: %s", r->root_name, r->root_path);

				BROWSER_ADD(row, name_cell(text));
			}
			BROWSER_ADD(row, name_cell(r->server_path != NULL ? r->server_path : ""));
			BROWSER_ADD(row, name_cell(r->library != NULL ? r->library : ""));
			if (r->reach == 2) {
				BROWSER_ADD(row, state_cell(axis_web_browser_badge("Told", "badge-succeeded")));
			} else if (r->reach == 1) {
				words = g_strdup_printf("Not mapped to a %s library", server_label(r->server_kind));
				BROWSER_ADD(row, state_cell(axis_web_browser_badge(words, "badge-warn")));
			} else {
				BROWSER_ADD(row, state_cell(axis_web_browser_badge("No path mapping reaches it", "badge-warn")));
			}
			BROWSER_ADD(body, row);
		}
		BROWSER_ADD(table, body);
		BROWSER_ADD(card, table);
	}
	BROWSER_ADD(content, card);

	card = axis_web_storage_card("Recent rescan requests");
	BROWSER_ADD(card, axis_web_storage_text("help", "A folder is sent once it has been quiet for the time set in "
		"Settings, so a movie with its subtitles is one request. A request that fails is tried again a few times "
		"within the hour and then dropped; an import is never held up or failed by a server that does not answer. "
		"When a path mapping is added or removed, the requests of the last seven days that no mapping reached, and "
		"that the map reaches now, are sent again by themselves; \"Send this request again\" sends any request that was not sent."));
	if (work->notifications->len == 0) {
		BROWSER_ADD(card, axis_web_storage_text(NULL, "Nothing has been sent yet."));
	} else {
		table = axis_web_table_new("rescan-requests", columns);
		body = htmx_tbody_new();
		for (i = 0; i < work->notifications->len; i++) {
			const AxisCompatNotifyInfo *n = g_ptr_array_index(work->notifications, i);
			g_autofree gchar *tries = g_strdup_printf("%d", n->attempts);
			g_autofree gchar *when = axis_web_storage_format_time(n->last_us, "never");
			g_autofree gchar *row_id = g_strdup_printf("rescan-%" G_GINT64_FORMAT, n->id);
			HtmxTr *row = htmx_tr_new();
			HtmxTd *first = htmx_td_new();
			HtmxSpan *kind = htmx_span_new();

			htmx_element_set_id(HTMX_ELEMENT(row), row_id);
			htmx_node_set_text_content(HTMX_NODE(kind), server_label(n->server_kind));
			BROWSER_ADD(first, kind);
			axis_web_notice_put(&notice, HTMX_NODE(first), row_id);
			BROWSER_ADD(row, first);
			BROWSER_ADD(row, name_cell(n->folder));
			BROWSER_ADD(row, text_cell(NULL, change_label(n->change)));
			BROWSER_ADD(row, text_cell("num", tries));
			BROWSER_ADD(row, text_cell(NULL, when));
			BROWSER_ADD(row, state_cell(notify_state_badge(n->state)));
			BROWSER_ADD(row, name_cell(n->note != NULL ? n->note : ""));
			{
				HtmxTd *cell = htmx_td_new();

				/* Not sent: no mapping reached it, the tries ran out, or the
				 * server was not to be told then. */
				if (g_strcmp0(n->state, "unmapped") == 0 || g_strcmp0(n->state, "dropped") == 0 ||
				    g_strcmp0(n->state, "skipped") == 0) {
					g_autofree gchar *action = g_strdup_printf("/compat/rescans/%" G_GINT64_FORMAT "/again", n->id);

					BROWSER_ADD(cell, action_form(request, action, "Send this request again"));
				}
				BROWSER_ADD(row, cell);
			}
			BROWSER_ADD(body, row);
		}
		BROWSER_ADD(table, body);
		BROWSER_ADD(card, table);
	}
	BROWSER_ADD(content, card);
	axis_web_notice_end(&notice);
	return page_response(request, "Player compatibility: rescan requests", content);
}

/* ── Actions ────────────────────────────────────────────────────── */

static HtmxResponse *
test_done(
	AxisWebCompat *self,
	HtmxRequest   *request,
	Work          *work
){
	g_autoptr(HtmxDiv) content = page_start("/compat", "Player compatibility");
	HtmxNode *card = axis_web_storage_card("Connection test");

	if (work->test.ok) {
		g_autofree gchar *text = g_strdup_printf("Connected to %s%s%s.",
			work->test.name != NULL ? work->test.name : "the media server",
			work->test.version != NULL ? ", version " : "", work->test.version != NULL ? work->test.version : "");

		axis_web_storage_audit(request, "compat connection test: succeeded");
		BROWSER_ADD(card, axis_web_auth_notice(FALSE, text));
	} else {
		g_autofree gchar *text = g_strdup_printf("The connection failed: %s",
			work->test.message != NULL ? work->test.message : "no reason was given");

		axis_web_storage_audit(request, "compat connection test: failed");
		BROWSER_ADD(card, axis_web_auth_notice(TRUE, text));
	}
	BROWSER_ADD(card, axis_web_browser_link("/compat", "Back to Player compatibility"));
	BROWSER_ADD(content, card);
	return page_response(request, "Player compatibility", content);
}

static HtmxResponse *
action_done(
	AxisWebCompat *self,
	HtmxRequest   *request,
	Work          *work
){
	g_autofree gchar *where = NULL;

	switch (work->kind) {
	case ACTION_POLL:
		if (work->error != NULL)
			return axis_web_internal_error(request, work->error);
		axis_web_storage_audit(request, "compat poll requested: job=%" G_GINT64_FORMAT, work->job_id);
		return axis_web_place_redirect(request, "/compat", "poll-requested", CONNECTION_PLACE);
	case ACTION_HISTORY:
		if (work->error != NULL)
			return axis_web_internal_error(request, work->error);
		axis_web_storage_audit(request, "compat history import requested: job=%" G_GINT64_FORMAT, work->job_id);
		return axis_web_place_redirect(request, "/compat", "history-requested", CONNECTION_PLACE);
	case ACTION_MAP_ADD:
		if (work->error != NULL && work->problem == NULL)
			return axis_web_internal_error(request, work->error);
		if (work->problem != NULL) {
			HtmxResponse *response;
			g_autoptr(HtmxDiv) content = page_start("/compat/mapping",
				"Player compatibility: media server libraries and paths");

			BROWSER_ADD(content, axis_web_auth_notice(TRUE, work->problem));
			BROWSER_ADD(content, axis_web_browser_link("/compat/mapping", "Back to Media server libraries and paths"));
			response = page_response(request, "Player compatibility: media server libraries and paths", content);
			htmx_response_set_status(response, 400);
			return response;
		}
		axis_web_storage_audit(request, "compat path map entry added");
		return axis_web_place_redirect(request, "/compat/mapping", "mapping-added", "path-map");
	case ACTION_MAP_REMOVE:
		if (work->error != NULL)
			return axis_web_internal_error(request, work->error);
		axis_web_storage_audit(request, "compat path map entry %s removed", work->id);
		return axis_web_place_redirect(request, "/compat/mapping", "mapping-removed", "path-map");
	case ACTION_ASK_AGAIN:
		if (g_error_matches(work->error, AXIS_COMPAT_ERROR, AXIS_COMPAT_ERROR_NOT_FOUND))
			return not_found(request);
		if (g_error_matches(work->error, AXIS_COMPAT_ERROR, AXIS_COMPAT_ERROR_INVALID))
			return axis_web_error_respond(request, 409, "Not asked again", work->error->message);
		if (work->error != NULL)
			return axis_web_internal_error(request, work->error);
		axis_web_storage_audit(request, "compat rescan request %s asked again", work->id);
		where = g_strdup_printf("rescan-%s", work->id);
		return axis_web_place_redirect(request, "/compat/rescans", work->asked == AXIS_COMPAT_ASK_AGAIN_WAITING ?
			"asked-again" : "already-waiting", where);
	default:
		if (work->error != NULL) {
			if (g_error_matches(work->error, AXIS_COMPAT_ERROR, AXIS_COMPAT_ERROR_NOT_FOUND))
				return not_found(request);
			return axis_web_internal_error(request, work->error);
		}
		where = handle_href(work->id);
		axis_web_storage_audit(request, "compat device %s", work->flag ? "pinned" : "unpinned");
		return axis_web_place_redirect(request, where, work->flag ? "pinned" : "unpinned", "device");
	}
}

static HtmxResponse *
page_done(
	AxisWebCompat *self,
	HtmxRequest   *request,
	gpointer       data
){
	Work *work = data;

	switch (work->kind) {
	case PAGE_OVERVIEW:
		return overview_done(self, request, work);
	case PAGE_DEVICES:
		return devices_done(self, request, work);
	case PAGE_DEVICE:
		return device_done(self, request, work);
	case PAGE_VERDICTS:
		return verdicts_done(self, request, work);
	case PAGE_FAILURES:
		return failures_done(self, request, work);
	case PAGE_MAPPING:
		return mapping_done(self, request, work);
	case PAGE_RESCANS:
		return rescans_done(self, request, work);
	case ACTION_TEST:
		return test_done(self, request, work);
	default:
		return action_done(self, request, work);
	}
}

static HtmxResponse *
start(
	gpointer     user_data,
	HtmxRequest *request,
	Work        *work
){
	return axis_web_compat_dispatch(user_data, request, work_run, page_done, work, work_free);
}

static Work *
work_new(Kind kind)
{
	Work *work = g_new0(Work, 1);

	work->kind = kind;
	return work;
}

#define SIMPLE_HANDLER(name, kind_value) \
static HtmxResponse * \
name(HtmxRequest *request, GHashTable *params, gpointer user_data) \
{ \
	return start(user_data, request, work_new(kind_value)); \
}

SIMPLE_HANDLER(handle_overview, PAGE_OVERVIEW)
SIMPLE_HANDLER(handle_devices, PAGE_DEVICES)
SIMPLE_HANDLER(handle_verdicts, PAGE_VERDICTS)
SIMPLE_HANDLER(handle_failures, PAGE_FAILURES)
SIMPLE_HANDLER(handle_mapping, PAGE_MAPPING)
SIMPLE_HANDLER(handle_rescans, PAGE_RESCANS)
SIMPLE_HANDLER(handle_poll, ACTION_POLL)
SIMPLE_HANDLER(handle_history, ACTION_HISTORY)

/* The kind of server a form names (Plex's forms name none), or %NULL when it
 * is not one. */
static const gchar *
server_of(HtmxRequest *request)
{
	const gchar *name = htmx_request_get_form_value(request, "server");

	if (name == NULL || name[0] == '\0')
		return AXIS_COMPAT_KIND_PLEX;
	return axis_compat_kind_label(name) != NULL ? name : NULL;
}

static HtmxResponse *
handle_test(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	const gchar *server = server_of(request);
	Work *work;

	if (server == NULL)
		return axis_web_error_respond(request, 400, "Bad request", "Choose a kind of media server.");
	work = work_new(ACTION_TEST);
	work->server = g_strdup(server);
	return start(user_data, request, work);
}

static HtmxResponse *
handle_device(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	const gchar *id = g_hash_table_lookup(params, "id");
	Work *work;

	if (id == NULL || !axis_compat_handle_split(id, NULL, NULL))
		return not_found(request);
	work = work_new(PAGE_DEVICE);
	work->id = g_strdup(id);
	return start(user_data, request, work);
}

static HtmxResponse *
handle_pin(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	const gchar *id = g_hash_table_lookup(params, "id");
	const gchar *value = htmx_request_get_form_value(request, "pinned");
	Work *work;

	if (id == NULL || !axis_compat_handle_split(id, NULL, NULL))
		return not_found(request);
	if (g_strcmp0(value, "yes") != 0 && g_strcmp0(value, "no") != 0)
		return axis_web_error_respond(request, 400, "Bad request", "Choose whether to always consider the device.");
	work = work_new(ACTION_PIN);
	work->id = g_strdup(id);
	work->flag = g_strcmp0(value, "yes") == 0;
	return start(user_data, request, work);
}

static HtmxResponse *
handle_map_add(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	const gchar *server_path = htmx_request_get_form_value(request, "server_path");
	const gchar *axis_path = htmx_request_get_form_value(request, "axis_path");
	const gchar *server = server_of(request);
	Work *work;

	if (server_path == NULL || axis_path == NULL || strlen(server_path) > 4096 || strlen(axis_path) > 4096)
		return axis_web_error_respond(request, 400, "Bad request", "Both paths are needed.");
	if (server == NULL)
		return axis_web_error_respond(request, 400, "Bad request", "Choose a kind of media server.");
	work = work_new(ACTION_MAP_ADD);
	work->server = g_strdup(server);
	work->server_path = g_strdup(server_path);
	work->axis_path = g_strdup(axis_path);
	return start(user_data, request, work);
}

static HtmxResponse *
handle_map_remove(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	const gchar *id = g_hash_table_lookup(params, "id");
	Work *work;

	if (id == NULL || id[0] == '\0' || strspn(id, "0123456789") != strlen(id) || strlen(id) > 18)
		return not_found(request);
	work = work_new(ACTION_MAP_REMOVE);
	work->id = g_strdup(id);
	return start(user_data, request, work);
}

static HtmxResponse *
handle_ask_again(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	const gchar *id = g_hash_table_lookup(params, "id");
	Work *work;

	if (id == NULL || id[0] == '\0' || strspn(id, "0123456789") != strlen(id) || strlen(id) > 18)
		return not_found(request);
	work = work_new(ACTION_ASK_AGAIN);
	work->id = g_strdup(id);
	return start(user_data, request, work);
}

void
axis_web_compat_register_pages(
	AxisWebCompat *self,
	AxisWeb       *web
){
	/* Every route is an administrator's: who plays what on which device is
	 * personal. */
	axis_web_route_full(web, HTMX_METHOD_GET, "/compat", AXIS_WEB_ROUTE_ADMIN, handle_overview, self);
	axis_web_route_full(web, HTMX_METHOD_GET, "/compat/devices", AXIS_WEB_ROUTE_ADMIN, handle_devices, self);
	axis_web_route_full(web, HTMX_METHOD_GET, "/compat/devices/:id", AXIS_WEB_ROUTE_ADMIN, handle_device, self);
	axis_web_route_full(web, HTMX_METHOD_GET, "/compat/verdicts", AXIS_WEB_ROUTE_ADMIN, handle_verdicts, self);
	axis_web_route_full(web, HTMX_METHOD_GET, "/compat/failures", AXIS_WEB_ROUTE_ADMIN, handle_failures, self);
	axis_web_route_full(web, HTMX_METHOD_GET, "/compat/mapping", AXIS_WEB_ROUTE_ADMIN, handle_mapping, self);
	axis_web_route_full(web, HTMX_METHOD_GET, "/compat/rescans", AXIS_WEB_ROUTE_ADMIN, handle_rescans, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/compat/test", AXIS_WEB_ROUTE_ADMIN, handle_test, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/compat/poll", AXIS_WEB_ROUTE_ADMIN, handle_poll, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/compat/history", AXIS_WEB_ROUTE_ADMIN, handle_history, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/compat/mapping/add", AXIS_WEB_ROUTE_ADMIN, handle_map_add, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/compat/mapping/:id/remove", AXIS_WEB_ROUTE_ADMIN, handle_map_remove,
		self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/compat/devices/:id/pin", AXIS_WEB_ROUTE_ADMIN, handle_pin, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/compat/rescans/:id/again", AXIS_WEB_ROUTE_ADMIN, handle_ask_again,
		self);
}
