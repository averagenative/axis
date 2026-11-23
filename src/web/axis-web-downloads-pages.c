/*
 * axis-web-downloads-pages.c - the Downloads page: what is in progress
 * (live), what finished lately, and removing one
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A download in progress is one row (id download-N) that the live events
 * replace whole; a download that starts or stops being in progress makes
 * both lists fetch themselves again.  The rows of administrators carry an
 * action, so they go on a topic of their own.  A client that cannot be
 * reached leaves its downloads as they were last seen, and their rows say
 * how long ago that was.
 */

#include "web/axis-web-downloads-private.h"

#include <string.h>

#include "db/axis-db-pool.h"
#include "web/axis-web-auth-private.h"
#include "web/axis-web-browser-private.h"
#include "web/axis-web-storage-private.h"

#define ADD axis_web_storage_add

/* A row's state counts as old when the client was last heard this long
 * ago (several polls), and the row says so. */
#define STALE_US (30 * G_USEC_PER_SEC)

#define RECENT_LIMIT 50

/* A row's link, with a name that says which row it acts on. */
static HtmxNode *
row_link(
	const gchar *href,
	const gchar *text,
	const gchar *action,
	const gchar *name
){
	HtmxNode *link = axis_web_browser_link(href, text);
	g_autofree gchar *label = g_strdup_printf("%s %s", action, name != NULL ? name : "");

	htmx_element_set_attribute(HTMX_ELEMENT(link), "aria-label", label);
	return link;
}

/* Block and search again (add-block-release): for a download Axis grabbed
 * for a movie or a series, the link to that module's confirmation, which
 * writes nothing (web/axis-web-block.h); %NULL for any other download.  The
 * modules own the blocklists, so the page is theirs. */
static HtmxNode *
block_link(const AxisDownloadInfo *info)
{
	const gchar *prefix = g_strcmp0(info->module, "movies") == 0 ? "/films" :
		g_strcmp0(info->module, "tv") == 0 ? "/series" : NULL;
	g_autofree gchar *href = NULL;

	if (prefix == NULL || info->title_id <= 0)
		return NULL;
	href = g_strdup_printf("%s/block-release?download=%" G_GINT64_FORMAT, prefix, info->id);
	return row_link(href, info->state == AXIS_DOWNLOAD_IMPORTED ? "Block this release" : "Block and search again",
		"Block the release of", info->title_label != NULL ? info->title_label : info->release_name);
}

/* Delete this download (add-replace-grab-and-delete-download, slice B): for a
 * finished download whose import is blocked or partly done, the link to that
 * import's confirmation, which writes nothing and says what would go.  A
 * link only: the import's page is where its reasons and its other ways out
 * are, and the import module decides and deletes. */
static HtmxNode *
delete_link(
	const AxisDownloadInfo *info,
	gint64                  import_id
){
	g_autofree gchar *href = NULL;

	if (import_id <= 0)
		return NULL;
	href = g_strdup_printf("/imports/%" G_GINT64_FORMAT "/delete-download", import_id);
	return row_link(href, "Delete this download", "Delete the download of", info->title_label != NULL ?
		info->title_label : info->release_name);
}

/* The actions of a row: each on a line of its own. */
static HtmxNode *
actions_cell(
	HtmxNode *first,
	HtmxNode *second,
	HtmxNode *third
){
	HtmxTd *cell = htmx_td_new();

	if (first != NULL)
		ADD(cell, first);
	if (second != NULL) {
		HtmxSpan *line = htmx_span_new();

		htmx_element_add_class(HTMX_ELEMENT(line), "range-detail");
		ADD(line, second);
		ADD(cell, line);
	}
	if (third != NULL) {
		HtmxSpan *line = htmx_span_new();

		htmx_element_add_class(HTMX_ELEMENT(line), "range-detail");
		ADD(line, third);
		ADD(cell, line);
	}
	return HTMX_NODE(cell);
}

/* ── Pieces ─────────────────────────────────────────────────────── */

gchar *
axis_web_downloads_age_text(gint64 us)
{
	gint64 seconds = us > 0 ? (g_get_real_time() - us) / G_USEC_PER_SEC : -1;

	if (seconds < 0)
		return g_strdup("never");
	if (seconds < 10)
		return g_strdup("just now");
	if (seconds < 120)
		return g_strdup_printf("%" G_GINT64_FORMAT " seconds ago", seconds);
	if (seconds < 7200)
		return g_strdup_printf("%" G_GINT64_FORMAT " minutes ago", seconds / 60);
	if (seconds < 2 * 86400)
		return g_strdup_printf("%" G_GINT64_FORMAT " hours ago", seconds / 3600);
	return g_strdup_printf("%" G_GINT64_FORMAT " days ago", seconds / 86400);
}

/* How long, as "7 minutes" or "2 hours": for a sentence that says "for ...". */
static gchar *
duration_text(gint64 us)
{
	gint64 seconds = us > 0 ? (g_get_real_time() - us) / G_USEC_PER_SEC : 0;
	gint64 n;

	if (seconds < 120)
		return g_strdup("a minute");
	if (seconds < 7200) {
		n = seconds / 60;
		return g_strdup_printf("%" G_GINT64_FORMAT " minutes", n);
	}
	if (seconds < 2 * 86400) {
		n = seconds / 3600;
		return g_strdup_printf("%" G_GINT64_FORMAT " hours", n);
	}
	n = seconds / 86400;
	return g_strdup_printf("%" G_GINT64_FORMAT " days", n);
}

/* A help or notice line made of words, a link and more words. */
static HtmxNode *
line_with_link(
	const gchar *css_class,
	const gchar *before,
	const gchar *href,
	const gchar *link_text,
	const gchar *after
){
	HtmxP *line = htmx_p_new();
	HtmxSpan *head = htmx_span_new();

	htmx_element_add_class(HTMX_ELEMENT(line), css_class);
	htmx_node_set_text_content(HTMX_NODE(head), before);
	ADD(line, head);
	ADD(line, axis_web_browser_link(href, link_text));
	if (after != NULL) {
		HtmxSpan *tail = htmx_span_new();

		htmx_node_set_text_content(HTMX_NODE(tail), after);
		ADD(line, tail);
	}
	return HTMX_NODE(line);
}

HtmxNode *
axis_web_downloads_bar(gdouble fraction)
{
	HtmxNode *svg = htmx_node_new("svg");
	HtmxNode *track = htmx_node_new("rect");
	HtmxNode *done = htmx_node_new("rect");
	guint permille = (guint)(CLAMP(fraction, 0.0, 1.0) * 1000.0 + 0.5);
	g_autofree gchar *label = g_strdup_printf("%u%% done", permille / 10);
	g_autofree gchar *width = g_strdup_printf("%u.%u", permille / 10, permille % 10);

	htmx_element_add_class(HTMX_ELEMENT(svg), "space-bar");
	htmx_element_set_attribute(HTMX_ELEMENT(svg), "viewBox", "0 0 100 4");
	htmx_element_set_attribute(HTMX_ELEMENT(svg), "preserveAspectRatio", "none");
	htmx_element_set_attribute(HTMX_ELEMENT(svg), "role", "img");
	htmx_element_set_attribute(HTMX_ELEMENT(svg), "aria-label", label);
	htmx_element_add_class(HTMX_ELEMENT(track), "space-bar-track");
	htmx_element_set_attribute(HTMX_ELEMENT(track), "width", "100");
	htmx_element_set_attribute(HTMX_ELEMENT(track), "height", "4");
	htmx_element_add_class(HTMX_ELEMENT(done), "space-bar-used");
	htmx_element_set_attribute(HTMX_ELEMENT(done), "width", width);
	htmx_element_set_attribute(HTMX_ELEMENT(done), "height", "4");
	ADD(svg, track);
	ADD(svg, done);
	return svg;
}

HtmxNode *
axis_web_downloads_client_badge(AxisDownloadClientState state)
{
	static const gchar *const css[] = { "badge-note", "badge-succeeded", "badge-failed", "badge-failed", "badge-warn" };

	return axis_web_browser_badge(axis_download_client_state_label(state), css[CLAMP((gint)state, 0, 4)]);
}

const gchar *
axis_web_downloads_label(const AxisDownloadInfo *info)
{
	return info->title_label != NULL ? info->title_label : info->release_name;
}

static HtmxNode *
cell_text(const gchar *text)
{
	return HTMX_NODE(htmx_td_new_with_text(text != NULL ? text : ""));
}

static HtmxNode *
cell_node(HtmxNode *child)
{
	HtmxTd *cell = htmx_td_new();

	if (child != NULL)
		ADD(cell, child);
	return HTMX_NODE(cell);
}

static gchar *
size_text(gint64 bytes)
{
	return bytes >= 0 ? axis_size_format((guint64)bytes) : g_strdup("");
}

static gchar *
rate_text(gint64 bps)
{
	g_autofree gchar *size = NULL;

	if (bps <= 0)
		return g_strdup("");
	size = axis_size_format((guint64)bps);
	return g_strdup_printf("%s/s", size);
}

static gchar *
eta_text(gint64 seconds)
{
	if (seconds <= 0)
		return g_strdup("");
	if (seconds < 60)
		return g_strdup("under a minute");
	if (seconds < 3600)
		return g_strdup_printf("%" G_GINT64_FORMAT " min", (seconds + 59) / 60);
	if (seconds < 86400)
		return g_strdup_printf("%" G_GINT64_FORMAT " h %02" G_GINT64_FORMAT " min", seconds / 3600,
			(seconds % 3600) / 60);
	return g_strdup_printf("%" G_GINT64_FORMAT " days", (seconds + 43200) / 86400);
}

/* The release, and below it what it is for when that is known. */
static HtmxNode *
name_cell(const AxisDownloadInfo *info)
{
	HtmxTd *cell = htmx_td_new();
	HtmxBdi *name = htmx_bdi_new();

	ADD(name, axis_web_breakable(axis_web_downloads_label(info)));
	ADD(cell, name);
	if (info->title_label != NULL) {
		HtmxSpan *release = htmx_span_new();
		HtmxBdi *text = htmx_bdi_new();

		htmx_element_add_class(HTMX_ELEMENT(release), "range-detail");
		ADD(text, axis_web_breakable(info->release_name));
		ADD(release, text);
		ADD(cell, release);
	}
	return HTMX_NODE(cell);
}

static const AxisWebColumn active_columns[] = {
	{ "Download", AXIS_WEB_COL_NAME },
	{ "Download client", AXIS_WEB_COL_TEXT },
	{ "Size", AXIS_WEB_COL_SIZE },
	{ "Progress", AXIS_WEB_COL_WIDE },
	{ "Speed", AXIS_WEB_COL_NUMBER },
	{ "Time left", AXIS_WEB_COL_NUMBER },
	{ "State", AXIS_WEB_COL_STATE },
	{ NULL, 0 }
};

static const AxisWebColumn active_admin_columns[] = {
	{ "Download", AXIS_WEB_COL_NAME },
	{ "Download client", AXIS_WEB_COL_TEXT },
	{ "Size", AXIS_WEB_COL_SIZE },
	{ "Progress", AXIS_WEB_COL_WIDE },
	{ "Speed", AXIS_WEB_COL_NUMBER },
	{ "Time left", AXIS_WEB_COL_NUMBER },
	{ "State", AXIS_WEB_COL_STATE },
	{ "", AXIS_WEB_COL_ACTIONS },
	{ NULL, 0 }
};

const AxisWebColumn *
axis_web_downloads_active_columns(gboolean admin)
{
	return admin ? active_admin_columns : active_columns;
}

/* How far, as words and a bar; and what stands in its way. */
static HtmxNode *
progress_cell(const AxisDownloadInfo *info)
{
	HtmxTd *cell = htmx_td_new();
	HtmxDiv *box = htmx_div_new();
	HtmxSpan *figures = htmx_span_new();
	gdouble progress = axis_download_info_progress(info);
	g_autofree gchar *done = size_text(info->done_bytes);
	g_autofree gchar *text = NULL;

	htmx_element_add_class(HTMX_ELEMENT(box), "download-progress");
	if (info->state == AXIS_DOWNLOAD_SENDING)
		text = g_strdup("Sending to the download client");
	else if (progress >= 0)
		text = g_strdup_printf("%.0f%%, %s", progress * 100.0, done);
	else
		text = g_strdup("Not known yet");
	htmx_node_set_text_content(HTMX_NODE(figures), text);
	ADD(box, figures);
	ADD(box, axis_web_downloads_bar(progress >= 0 ? progress : 0.0));
	ADD(cell, box);
	if (info->problem != NULL) {
		HtmxSpan *problem = htmx_span_new();

		htmx_element_add_class(HTMX_ELEMENT(problem), "range-detail");
		htmx_element_add_class(HTMX_ELEMENT(problem), "download-problem");
		htmx_node_set_text_content(HTMX_NODE(problem), info->problem);
		ADD(cell, problem);
	}
	return HTMX_NODE(cell);
}

/* The state, and how old it is when the client has gone quiet. */
static HtmxNode *
state_cell(const AxisDownloadInfo *info)
{
	HtmxTd *cell = htmx_td_new();
	const gchar *css = "badge-running";
	const gchar *label = axis_download_state_label(info->state);
	gboolean quiet = info->client_state == AXIS_DOWNLOAD_CLIENT_UNREACHABLE ||
		info->client_state == AXIS_DOWNLOAD_CLIENT_REFUSED || info->client_state == AXIS_DOWNLOAD_CLIENT_FAILING;

	if (info->state == AXIS_DOWNLOAD_PAUSED || info->state == AXIS_DOWNLOAD_QUEUED ||
	    info->state == AXIS_DOWNLOAD_SENDING)
		css = "badge-note";
	if (info->problem != NULL && info->client_path != NULL) {
		label = "Waiting";
		css = "badge-warn";
	} else if (info->stalled) {
		label = "Stalled";
		css = "badge-warn";
	}
	ADD(cell, axis_web_browser_badge(label, css));
	if (quiet || (info->seen_us > 0 && g_get_real_time() - info->seen_us > STALE_US)) {
		HtmxSpan *age = htmx_span_new();
		g_autofree gchar *when = axis_web_downloads_age_text(info->seen_us);
		g_autofree gchar *text = info->seen_us > 0 ? g_strdup_printf("last seen %s", when) :
			g_strdup("not seen at the download client yet");

		htmx_element_add_class(HTMX_ELEMENT(age), "range-detail");
		htmx_node_set_text_content(HTMX_NODE(age), text);
		ADD(cell, age);
	}
	return HTMX_NODE(cell);
}

HtmxNode *
axis_web_downloads_active_row(
	const AxisDownloadInfo *info,
	gboolean                admin
){
	HtmxTr *row = htmx_tr_new();
	g_autofree gchar *id = g_strdup_printf("download-%" G_GINT64_FORMAT, info->id);
	g_autofree gchar *size = size_text(info->size_bytes);
	g_autofree gchar *rate = rate_text(info->rate_bps);
	g_autofree gchar *eta = eta_text(info->eta_s);

	htmx_element_set_id(HTMX_ELEMENT(row), id);
	ADD(row, name_cell(info));
	ADD(row, cell_text(info->client_name));
	ADD(row, cell_text(size));
	ADD(row, progress_cell(info));
	ADD(row, cell_text(rate));
	ADD(row, cell_text(eta));
	ADD(row, state_cell(info));
	if (admin) {
		g_autofree gchar *href = g_strdup_printf("/downloads/remove/%" G_GINT64_FORMAT, info->id);

		ADD(row, actions_cell(info->client_ref != NULL ? row_link(href, "Remove", "Remove download",
			axis_web_downloads_label(info)) : NULL, block_link(info), NULL));
	}
	return HTMX_NODE(row);
}

static const AxisWebColumn recent_columns[] = {
	{ "Download", AXIS_WEB_COL_NAME },
	{ "Download client", AXIS_WEB_COL_TEXT },
	{ "Size", AXIS_WEB_COL_SIZE },
	{ "State", AXIS_WEB_COL_STATE },
	{ "When", AXIS_WEB_COL_DATE },
	{ "Location or reason", AXIS_WEB_COL_WIDE },
	{ NULL, 0 }
};

static const AxisWebColumn recent_admin_columns[] = {
	{ "Download", AXIS_WEB_COL_NAME },
	{ "Download client", AXIS_WEB_COL_TEXT },
	{ "Size", AXIS_WEB_COL_SIZE },
	{ "State", AXIS_WEB_COL_STATE },
	{ "When", AXIS_WEB_COL_DATE },
	{ "Location or reason", AXIS_WEB_COL_WIDE },
	{ "", AXIS_WEB_COL_ACTIONS },
	{ NULL, 0 }
};

static HtmxNode *
recent_row(
	const AxisDownloadInfo *info,
	gboolean                admin,
	gint64                  import_id
){
	static const gchar *const css[AXIS_DOWNLOAD_N_STATES] = {
		"badge-note", "badge-note", "badge-running", "badge-note", "badge-succeeded", "badge-failed", "badge-warn",
		"badge-note", "badge-succeeded"
	};
	HtmxTr *row = htmx_tr_new();
	g_autofree gchar *size = size_text(info->size_bytes);
	g_autofree gchar *when = axis_web_format_time(info->finished_us > 0 ? info->finished_us : info->updated_us, "");
	g_autofree gchar *where = NULL;
	HtmxTd *place = htmx_td_new();

	ADD(row, name_cell(info));
	ADD(row, cell_text(info->client_name));
	ADD(row, cell_text(size));
	ADD(row, cell_node(axis_web_browser_badge(axis_download_state_label(info->state),
		css[CLAMP((gint)info->state, 0, AXIS_DOWNLOAD_N_STATES - 1)])));
	ADD(row, cell_text(when));
	if ((info->state == AXIS_DOWNLOAD_COMPLETED || info->state == AXIS_DOWNLOAD_IMPORTED) && info->rel_path != NULL) {
		HtmxSpan *volume = htmx_span_new();
		HtmxCode *path = htmx_code_new();
		g_autofree gchar *on = g_strdup_printf("%s: ", info->volume_name != NULL ? info->volume_name :
			"a disk Axis no longer knows");

		htmx_node_set_text_content(HTMX_NODE(volume), on);
		ADD(path, axis_web_breakable(info->rel_path));
		ADD(place, volume);
		ADD(place, path);
		/* Left at its client on purpose (a torrent Axis does not ask the
		 * client to delete where it is now): why, and what to do. */
		if (g_strcmp0(info->folder_removal, "left") == 0 && info->folder_removal_text != NULL) {
			HtmxSpan *left = htmx_span_new();
			g_autofree gchar *shown = g_strconcat(" Left at the download client: ", info->folder_removal_text, NULL);

			htmx_element_add_class(HTMX_ELEMENT(left), "range-detail");
			ADD(left, axis_web_breakable(shown));
			ADD(place, left);
		}
	} else {
		const gchar *why = info->message != NULL ? info->message : info->problem;

		if (info->state == AXIS_DOWNLOAD_GONE && why == NULL)
			why = "No longer in the client, which did not say why.";
		if (info->state == AXIS_DOWNLOAD_REMOVED && why == NULL)
			why = "Removed from the client by Axis.";
		ADD(place, axis_web_breakable(why != NULL ? why : ""));
		/* What the client may have left of it, and what Axis did. */
		if (info->folder_removal != NULL) {
			HtmxSpan *files = htmx_span_new();
			const gchar *text = info->folder_removal_text != NULL ? info->folder_removal_text :
				g_strcmp0(info->folder_removal, "pending") == 0 ? "What the client kept of its files is looked at by "
				"the hourly cleanup, and deleted where the rules allow it." : "";
			g_autofree gchar *shown = g_strconcat(" Files left at the download client: ", text, NULL);

			htmx_element_add_class(HTMX_ELEMENT(files), "range-detail");
			ADD(files, axis_web_breakable(shown));
			ADD(place, files);
		}
	}
	ADD(row, place);
	if (admin) {
		g_autofree gchar *href = g_strdup_printf("/downloads/remove/%" G_GINT64_FORMAT, info->id);
		gboolean removable = info->client_ref != NULL && info->client_id > 0 &&
			info->state != AXIS_DOWNLOAD_REMOVED && info->state != AXIS_DOWNLOAD_GONE;

		/* A finished grab: its release can be blocked (downloaded and waiting
		 * for its import, or in the library already). */
		gboolean finished = info->state == AXIS_DOWNLOAD_COMPLETED || info->state == AXIS_DOWNLOAD_IMPORTED;

		ADD(row, actions_cell(removable ? row_link(href, "Remove", "Remove download",
			axis_web_downloads_label(info)) : NULL, finished ? block_link(info) : NULL,
			info->state == AXIS_DOWNLOAD_COMPLETED ? delete_link(info, import_id) : NULL));
	}
	return HTMX_NODE(row);
}

/* ── The lists ──────────────────────────────────────────────────── */

static HtmxNode *
list_box(
	const gchar *id,
	const gchar *part,
	const gchar *heading
){
	HtmxNode *card = axis_web_storage_card(heading);
	g_autofree gchar *url = g_strdup_printf("/downloads/list?part=%s", part);

	htmx_element_set_id(HTMX_ELEMENT(card), id);
	htmx_element_set_attribute(HTMX_ELEMENT(card), "hx-get", url);
	htmx_element_set_attribute(HTMX_ELEMENT(card), "hx-trigger",
		"sse:" AXIS_DOWNLOADS_EVENT_CHANGED " throttle:1s, htmx:sseOpen from:body");
	htmx_element_set_attribute(HTMX_ELEMENT(card), "hx-swap", "outerHTML");
	return card;
}

static HtmxNode *
active_list(
	GPtrArray *rows,
	gboolean   admin
){
	HtmxNode *card = list_box("downloads-active", "active", "In progress");
	HtmxNode *table;
	HtmxTbody *body;
	guint i;

	if (rows->len == 0) {
		HtmxP *line = htmx_p_new();
		HtmxSpan *words = htmx_span_new();
		HtmxSpan *tail = htmx_span_new();
		HtmxSpan *joint = htmx_span_new();

		htmx_element_add_class(HTMX_ELEMENT(line), "help");
		htmx_node_set_text_content(HTMX_NODE(words), "Nothing is downloading. Axis starts a download when it finds a "
			"release for a wanted movie or episode. See ");
		htmx_node_set_text_content(HTMX_NODE(joint), " and ");
		htmx_node_set_text_content(HTMX_NODE(tail), ".");
		ADD(line, words);
		ADD(line, axis_web_browser_link("/films/wanted", "Wanted movies"));
		ADD(line, joint);
		ADD(line, axis_web_browser_link("/series/wanted", "Wanted episodes"));
		ADD(line, tail);
		ADD(card, line);
		return card;
	}
	table = axis_web_table_new(admin ? "downloads-active-admin" : "downloads-active",
		axis_web_downloads_active_columns(admin));
	body = htmx_tbody_new();
	for (i = 0; i < rows->len; i++)
		ADD(body, axis_web_downloads_active_row(g_ptr_array_index(rows, i), admin));
	ADD(table, body);
	ADD(card, table);
	return card;
}

static HtmxNode *
recent_list(
	GPtrArray  *rows,
	gboolean    admin,
	GHashTable *imports
){
	HtmxNode *card = list_box("downloads-recent", "recent", "Finished lately");
	HtmxNode *table;
	HtmxTbody *body;
	guint i;

	if (rows->len == 0) {
		ADD(card, axis_web_storage_text("help", "No download has finished yet."));
		return card;
	}
	table = axis_web_table_new(admin ? "downloads-recent-admin" : "downloads-recent",
		admin ? recent_admin_columns : recent_columns);
	body = htmx_tbody_new();
	for (i = 0; i < rows->len; i++) {
		const AxisDownloadInfo *info = g_ptr_array_index(rows, i);
		const gint64 *import_id = imports != NULL ? g_hash_table_lookup(imports, &info->id) : NULL;

		ADD(body, recent_row(info, admin, import_id != NULL ? *import_id : 0));
	}
	ADD(table, body);
	ADD(card, table);
	return card;
}

/* The clients that have gone quiet, each said once above the lists. */
static void
quiet_notices(
	HtmxNode  *content,
	GPtrArray *clients
){
	guint i;

	for (i = 0; clients != NULL && i < clients->len; i++) {
		const AxisDownloadClientInfo *client = g_ptr_array_index(clients, i);
		g_autofree gchar *since = NULL;
		g_autofree gchar *href = NULL;
		g_autofree gchar *text = NULL;

		/* A client switched off is still followed while it has downloads in
		 * progress, so it is said of then too. */
		if ((!client->enabled && client->n_active == 0) || (client->state != AXIS_DOWNLOAD_CLIENT_UNREACHABLE &&
		                                                    client->state != AXIS_DOWNLOAD_CLIENT_REFUSED &&
		                                                    client->state != AXIS_DOWNLOAD_CLIENT_FAILING))
			continue;
		since = axis_web_downloads_age_text(client->last_contact_us);
		href = g_strdup_printf("/downloads/clients/%" G_GINT64_FORMAT, client->id);
		text = client->last_contact_us > 0 ?
			g_strdup_printf(" last answered properly %s. Its downloads show what was last known, and none is marked "
				"failed because of it. Check that it is running, then test it on its page.", since) :
			g_strdup(" has not answered properly yet. Its downloads show what was last known. Check that it is running, "
				"then test it on its page.");
		ADD(content, line_with_link("notice notice-warning", "", href, client->name, text));
	}
}

/* How long a client's list of Axis's downloads may not be whole before the
 * page says so (its own page says it at once). */
#define PARTIAL_NOTICE_S 3600

/* The clients whose list of Axis's downloads has not been whole for long:
 * what it does not show is kept as it was, never taken for gone, so a
 * download that is gone there stays as it was until it is whole again. */
static void
partial_notices(
	HtmxNode  *content,
	GPtrArray *clients
){
	gint64 now = g_get_real_time();
	guint i;

	for (i = 0; clients != NULL && i < clients->len; i++) {
		const AxisDownloadClientInfo *client = g_ptr_array_index(clients, i);
		g_autofree gchar *since = NULL;
		g_autofree gchar *href = NULL;
		g_autofree gchar *before = NULL;
		g_autofree gchar *text = NULL;

		if (client->partial_since_us <= 0 ||
		    now - client->partial_since_us < (gint64)PARTIAL_NOTICE_S * G_USEC_PER_SEC)
			continue;
		since = duration_text(client->partial_since_us);
		href = g_strdup_printf("/downloads/clients/%" G_GINT64_FORMAT, client->id);
		before = g_strdup_printf("Axis has not been able to read the whole download list of %s for %s: the list holds "
			"more than Axis reads, or an entry Axis cannot read. Downloads missing from the list keep their last known "
			"state, so Axis does not notice one that is gone. See ", client->name, since);
		text = g_strdup(" under Download clients.");
		ADD(content, line_with_link("notice notice-warning", before, href, client->name, text));
	}
}

/* What the Leftovers page last found in the clients' folders for Axis, when
 * anything: a line with the link (administrators). */
static void
leftovers_notice(
	HtmxNode  *content,
	GPtrArray *clients
){
	g_autofree gchar *text = NULL;
	HtmxSpan *words;
	HtmxP *line;
	guint total = 0;
	guint i;

	for (i = 0; clients != NULL && i < clients->len; i++)
		total += ((const AxisDownloadClientInfo *)g_ptr_array_index(clients, i))->n_leftovers;
	if (total == 0)
		return;
	line = htmx_p_new();
	htmx_element_add_class(HTMX_ELEMENT(line), "help");
	text = g_strdup_printf("%u %s in the download clients' folders for Axis %s to no download and no import, so none "
		"of it was imported: ", total, total == 1 ? "file or folder" : "files and folders",
		total == 1 ? "belongs" : "belong");
	words = htmx_span_new();
	htmx_node_set_text_content(HTMX_NODE(words), text);
	ADD(line, words);
	ADD(line, axis_web_browser_link("/downloads/leftovers", "Leftovers in the download folders"));
	ADD(content, line);
}

/* ── The page ───────────────────────────────────────────────────── */

typedef struct
{
	gchar     *done;
	gchar     *part;	/* the fragment asked for, or NULL for the page */
	gboolean   admin;
	GPtrArray *active;
	GPtrArray *recent;
	GPtrArray *clients;
	/* (nullable) For an administrator: the import that is blocked or partly
	 * done of each download that has one (gint64 download id -> gint64
	 * import id), for the row's "Delete this download". */
	GHashTable *imports;
	GError    *error;
} PageWork;

static void
page_work_free(gpointer data)
{
	PageWork *work = data;

	g_free(work->done);
	g_free(work->part);
	g_clear_pointer(&work->imports, g_hash_table_unref);
	g_clear_pointer(&work->active, g_ptr_array_unref);
	g_clear_pointer(&work->recent, g_ptr_array_unref);
	g_clear_pointer(&work->clients, g_ptr_array_unref);
	g_clear_error(&work->error);
	g_free(work);
}

static void
page_run(
	AxisWebDownloads *self,
	gpointer          data
){
	PageWork *work = data;

	if ((work->part == NULL || strcmp(work->part, "active") == 0) &&
	    !axis_downloads_list(self->downloads, AXIS_DOWNLOAD_LIST_ACTIVE, 500, &work->active, &work->error))
		return;
	if ((work->part == NULL || strcmp(work->part, "recent") == 0) &&
	    !axis_downloads_list(self->downloads, AXIS_DOWNLOAD_LIST_RECENT, RECENT_LIMIT, &work->recent, &work->error))
		return;
	/* The imports that wait for a person, by their download: one statement
	 * over the partial index import_attention (migration 0076), for an
	 * administrator's rows only.  A failure costs the links, not the page. */
	if (work->admin && work->recent != NULL) {
		g_autoptr(AxisDbResult) rows = NULL;
		g_autoptr(GError) local = NULL;
		guint i;

		if (axis_db_pool_exec(axis_downloads_get_pool(self->downloads), "SELECT download_id, id FROM import"
		        " WHERE state IN ('blocked', 'partial') AND download_id IS NOT NULL ORDER BY id LIMIT 2000", 0, NULL,
		        &rows, &local)) {
			work->imports = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_free);
			for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
				gint64 *download = g_new0(gint64, 1);
				gint64 *import = g_new0(gint64, 1);

				axis_db_result_get_int64(rows, i, 0, download);
				axis_db_result_get_int64(rows, i, 1, import);
				g_hash_table_replace(work->imports, download, import);
			}
		} else {
			g_debug("downloads: the imports that wait for a person could not be read: %s", local->message);
		}
	}
	if (work->part == NULL)
		axis_downloads_list_clients(self->downloads, &work->clients, &work->error);
}

static const gchar *
done_message(
	const gchar *key,
	gboolean    *error
){
	static const struct
	{
		const gchar *key;
		gboolean     error;
		const gchar *text;
	} messages[] = {
		{ "removed", FALSE, "The download was removed from its download client." },
		{ "removed-data", FALSE, "The download was removed from its download client, and the download client was asked "
			"to delete its files (SABnzbd deletes the files of a failed download, not of a completed one)." },
		{ "removed-left", FALSE, "The download was removed from its download client without its files. The download "
			"client says they are in a library folder, so it was not asked to delete them: they were left where they "
			"are. The download's line under \"Finished lately\" says where." },
		{ "gone", FALSE, "The client no longer had the download (something else removed it), so nothing was removed "
			"or deleted there; it is recorded as gone." },
		{ NULL, FALSE, NULL }
	};
	guint i;

	for (i = 0; key != NULL && messages[i].key != NULL; i++) {
		if (strcmp(key, messages[i].key) == 0) {
			*error = messages[i].error;
			return messages[i].text;
		}
	}
	return NULL;
}

static HtmxResponse *
fragment_response(HtmxNode *node)
{
	g_autoptr(HtmxResponse) response = htmx_response_new();
	g_autofree gchar *markup = NULL;

	axis_web_table_apply_roles(node);
	markup = htmx_element_render(HTMX_ELEMENT(node));
	htmx_response_set_content(response, markup);
	htmx_response_set_content_type(response, "text/html; charset=utf-8");
	return (HtmxResponse *)g_steal_pointer(&response);
}

static HtmxResponse *
page_done(
	AxisWebDownloads *self,
	HtmxRequest      *request,
	gpointer          data
){
	PageWork *work = data;
	g_autoptr(HtmxDiv) content = NULL;
	HtmxHeading *title;
	const gchar *message;
	gboolean is_error = FALSE;

	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	if (work->part != NULL) {
		g_autoptr(HtmxNode) node = strcmp(work->part, "active") == 0 ? active_list(work->active, work->admin) :
			recent_list(work->recent, work->admin, work->imports);

		return fragment_response(node);
	}
	content = htmx_div_new();
	title = htmx_h1_new();
	htmx_node_set_text_content(HTMX_NODE(title), "Downloads");
	ADD(content, title);
	message = done_message(work->done, &is_error);
	if (message != NULL)
		ADD(content, axis_web_auth_notice(is_error, message));
	if (work->clients != NULL && work->clients->len == 0) {
		/* With YouTube downloads listed below, "no client" would be wrong:
		 * said of the kinds an administrator sets up, and what these are. */
		gboolean builtin = axis_web_downloads_any_builtin(work->active) || axis_web_downloads_any_builtin(work->recent);
		const gchar *words = builtin ?
			"No download client for Usenet or torrents is set up yet. The downloads here are YouTube videos, which "
			"Axis downloads itself. " : "No download client is set up yet. ";
		g_autofree gchar *for_others = g_strconcat(words, "Ask an administrator to add one.", NULL);

		ADD(content, work->admin ?
			line_with_link("help", words, "/downloads/clients", "Add a download client", NULL) :
			axis_web_storage_text("help", for_others));
	}
	quiet_notices(HTMX_NODE(content), work->clients);
	partial_notices(HTMX_NODE(content), work->clients);
	if (work->admin)
		leftovers_notice(HTMX_NODE(content), work->clients);
	ADD(content, active_list(work->active, work->admin));
	ADD(content, recent_list(work->recent, work->admin, work->imports));
	axis_web_page_add_topic(request, work->admin ? AXIS_DOWNLOADS_TOPIC_ADMIN : AXIS_DOWNLOADS_TOPIC);
	return axis_web_page_respond(request, "Downloads", HTMX_NODE(content));
}

static HtmxResponse *
handle_page(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	PageWork *work = g_new0(PageWork, 1);
	const gchar *done = htmx_request_get_query_param(request, "done");
	gboolean is_error = FALSE;

	/* Only a known key is kept: nothing from the address is shown. */
	if (done_message(done, &is_error) != NULL)
		work->done = g_strdup(done);
	work->admin = axis_web_request_is_admin(request);
	return axis_web_downloads_dispatch(user_data, request, page_run, page_done, work, page_work_free);
}

static HtmxResponse *
handle_list(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	const gchar *part = htmx_request_get_query_param(request, "part");
	PageWork *work;

	if (part == NULL || (strcmp(part, "active") != 0 && strcmp(part, "recent") != 0))
		return axis_web_error_respond(request, 400, "Bad request", "There is no such list.");
	/* Nothing to refresh without htmx: send the browser to the page. */
	if (!htmx_request_is_htmx(request))
		return axis_web_auth_redirect(request, "/downloads");
	work = g_new0(PageWork, 1);
	work->part = g_strdup(part);
	work->admin = axis_web_request_is_admin(request);
	return axis_web_downloads_dispatch(user_data, request, page_run, page_done, work, page_work_free);
}

/* ── Removing ───────────────────────────────────────────────────── */

typedef struct
{
	gint64            id;
	gboolean          confirmed;
	gboolean          post;
	gboolean          delete_data;
	gboolean          leave;	/* posted from a confirmation that said its files are left */
	AxisDownloadInfo *info;
	/* A torrent its client says is in a library folder: its files are left
	 * whatever is ticked (the downloads layer's place rule).  On the
	 * confirmation, why they would be; after the post, what was recorded. */
	gchar            *left;
	GError           *error;
} RemoveWork;

static void
remove_work_free(gpointer data)
{
	RemoveWork *work = data;

	g_clear_pointer(&work->info, axis_download_info_free);
	g_clear_error(&work->error);
	g_free(work->left);
	g_free(work);
}

static void
remove_run(
	AxisWebDownloads *self,
	gpointer          data
){
	RemoveWork *work = data;

	if (!work->post) {
		if (axis_downloads_get(self->downloads, work->id, &work->info, &work->error))
			axis_downloads_removal_leaves(self->downloads, work->id, &work->left, &work->error);
		return;
	}
	/* The confirmation said its files are left: nothing is deleted, whatever
	 * else the form carries, and where it is now is recorded all the same. */
	if (work->leave)
		axis_downloads_remove_leaving(self->downloads, work->id, &work->left, &work->error);
	else
		axis_downloads_remove_full(self->downloads, work->id, work->delete_data, &work->left, &work->error);
}

static HtmxResponse *
remove_page(
	HtmxRequest            *request,
	const AxisDownloadInfo *info,
	const gchar            *left
){
	g_autoptr(HtmxDiv) content = htmx_div_new();
	HtmxHeading *title = htmx_h1_new();
	HtmxNode *card = axis_web_storage_card(axis_web_downloads_label(info));
	HtmxNode *facts = axis_web_browser_facts_new();
	g_autofree gchar *action = g_strdup_printf("/downloads/remove/%" G_GINT64_FORMAT, info->id);
	g_autofree gchar *size = size_text(info->size_bytes);
	g_autofree gchar *confirm = g_strdup_printf("Remove this download from %s", info->client_name);
	HtmxNode *form;
	HtmxDiv *actions = htmx_div_new();

	htmx_node_set_text_content(HTMX_NODE(title), "Remove a download");
	ADD(content, title);
	axis_web_browser_fact_node(facts, "Release", axis_web_browser_name_node(info->release_name));
	axis_web_browser_fact(facts, "Download client", info->client_name);
	axis_web_browser_fact(facts, "State", axis_download_state_label(info->state));
	axis_web_browser_fact(facts, "Size", size);
	ADD(card, facts);
	if (left == NULL) {
		ADD(card, axis_web_storage_text(NULL, info->protocol == AXIS_DOWNLOAD_YOUTUBE ?
			"Removing a YouTube download that is still running stops it and deletes what was downloaded so far. A video "
			"that is in the library already stays there." :
			info->protocol == AXIS_DOWNLOAD_USENET ?
			"A Usenet download is usually removed with its files once they are in the library." :
			"A torrent left in the client keeps seeding; removing its files stops that and deletes them at the "
			"client."));
	}
	form = axis_web_auth_post_form(request, action);
	htmx_element_add_class(HTMX_ELEMENT(form), "form");
	/* In a library folder now: the client is never asked to delete there, so
	 * the page says so in place of the offer. */
	if (left != NULL) {
		g_autofree gchar *words = g_strdup_printf("Its files will be left where they are: %s Axis removes the torrent "
			"from the client and does not ask the client to delete its files.", left);

		ADD(card, axis_web_storage_text(NULL, words));
		ADD(form, axis_web_auth_hidden("leave", "yes"));
	}
	/* Axis's own downloader keeps nothing of a download that is stopped. */
	if (info->protocol != AXIS_DOWNLOAD_YOUTUBE && left == NULL)
		ADD(form, axis_web_storage_checkbox("remove-data", "Also delete its files at the client", "delete_data", FALSE));
	ADD(form, axis_web_storage_checkbox("remove-confirm", confirm, "confirm", TRUE));
	/* In a row of actions, the button keeps its own width. */
	htmx_element_add_class(HTMX_ELEMENT(actions), "actions");
	ADD(actions, axis_web_auth_button("Remove this download", "button-danger"));
	ADD(form, actions);
	ADD(card, form);
	ADD(content, card);
	ADD(content, axis_web_browser_link("/downloads", "Back to Downloads"));
	return axis_web_page_respond(request, "Remove a download", HTMX_NODE(content));
}

static HtmxResponse *
remove_done(
	AxisWebDownloads *self,
	HtmxRequest      *request,
	gpointer          data
){
	RemoveWork *work = data;

	if (g_error_matches(work->error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_NOT_FOUND))
		return axis_web_error_respond(request, 404, "Not found", "There is no such download, or Axis did not add it.");
	if (work->post && g_error_matches(work->error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_GONE)) {
		axis_web_storage_audit(request, "download %" G_GINT64_FORMAT " was already gone from its client", work->id);
		return axis_web_auth_redirect(request, "/downloads?done=gone");
	}
	if (work->error != NULL && work->error->domain == AXIS_DOWNLOAD_ERROR) {
		g_autofree gchar *text = g_strdup_printf("The download was not removed: %s", work->error->message);

		return axis_web_error_respond(request, 409, "Download not removed", text);
	}
	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	if (!work->post)
		return remove_page(request, work->info, work->left);
	/* Asked with its files, and removed without them: said as it happened. */
	if (work->left != NULL) {
		axis_web_storage_audit(request, "download %" G_GINT64_FORMAT " removed from its client without its files, "
			"which are in a library folder", work->id);
		return axis_web_auth_redirect(request, "/downloads?done=removed-left");
	}
	axis_web_storage_audit(request, "download %" G_GINT64_FORMAT " removed from its client%s", work->id,
		work->delete_data ? " with its files" : "");
	return axis_web_auth_redirect(request, work->delete_data ? "/downloads?done=removed-data" :
		"/downloads?done=removed");
}

static HtmxResponse *
handle_remove(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	gint64 id = axis_web_storage_param_id(params);
	RemoveWork *work;
	gboolean post = htmx_request_get_method(request) == HTMX_METHOD_POST;

	if (id <= 0)
		return axis_web_error_respond(request, 404, "Not found", "There is no such download.");
	if (post && !axis_web_storage_form_yes(request, "confirm"))
		return axis_web_error_respond(request, 400, "Not confirmed", "Tick the box to remove the download.");
	work = g_new0(RemoveWork, 1);
	work->id = id;
	work->post = post;
	work->leave = post && axis_web_storage_form_yes(request, "leave");
	work->delete_data = post && !work->leave && axis_web_storage_form_yes(request, "delete_data");
	return axis_web_downloads_dispatch(user_data, request, remove_run, remove_done, work, remove_work_free);
}

void
axis_web_downloads_register_pages(
	AxisWebDownloads *self,
	AxisWeb          *web
){
	axis_web_route_full(web, HTMX_METHOD_GET, "/downloads", AXIS_WEB_ROUTE_DEFAULT, handle_page, self);
	axis_web_route_full(web, HTMX_METHOD_GET, "/downloads/list", AXIS_WEB_ROUTE_DEFAULT, handle_list, self);
	axis_web_route_full(web, HTMX_METHOD_GET, "/downloads/remove/:id", AXIS_WEB_ROUTE_ADMIN, handle_remove, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/downloads/remove/:id", AXIS_WEB_ROUTE_ADMIN, handle_remove, self);
}
