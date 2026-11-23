/*
 * axis-web-block.c - Block and search again: its confirmation and its post,
 * for every module that grabs (see axis-web-block.h)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The confirmation is read only.  The post goes through the module to
 * axis_acquire_block_grab(), which reuses the failure path for a download
 * that is still Axis's and only writes a blocklist entry for a release that
 * is in the library already.  Nothing here deletes or unlinks a file.
 */

#include "web/axis-web-block.h"

#include <string.h>

#include "web/axis-web-auth-private.h"
#include "web/axis-web-browser-private.h"
#include "web/axis-web-storage-private.h"

#define ADD axis_web_storage_add

typedef struct
{
	const AxisWebBlockInfo *info;
	gpointer                data;
} Registered;

typedef struct
{
	const Registered *reg;
	HtmxRequest      *request;
	HtmxResponse     *deferred;
	gboolean          post;
	gint64            owner_id;	/* as posted */
	gint64            grab_id;
	gint64            download_id;
	gint64            import_id;
	gchar            *user;
	const gchar      *done;	/* static, (nullable): what was just done */
	gboolean          cancelled;
	AxisWebBlockRead  read;
	GError           *error;
} Work;

static void
work_free(Work *work)
{
	g_clear_object(&work->request);
	g_clear_object(&work->deferred);
	g_free(work->user);
	g_free(work->read.title);
	g_free(work->read.back);
	g_free(work->read.blocklist);
	axis_acquire_block_grab_free(work->read.grab);
	g_clear_error(&work->error);
	g_free(work);
}

gchar *
axis_web_block_path(
	const gchar *prefix,
	const gchar *by,
	gint64       id
){
	return g_strdup_printf("%s/block-release?%s=%" G_GINT64_FORMAT, prefix, by, id);
}

static HtmxResponse *
failed(
	Work        *work,
	const gchar *heading
){
	guint status = work->reg->info->status(work->error);
	g_autofree gchar *message = NULL;

	if (status == 0)
		return axis_web_internal_error(work->request, work->error);
	message = g_strdup_printf("%c%s%s", g_ascii_toupper(work->error->message[0]), work->error->message + 1,
		g_str_has_suffix(work->error->message, ".") ? "" : ".");
	return axis_web_error_respond(work->request, status, heading, message);
}

/* ── The page ───────────────────────────────────────────────────── */

static const gchar *
done_text(const gchar *done)
{
	if (g_strcmp0(done, "cancelled") == 0)
		return "Blocked. The release is on the blocklist under your name, and Axis gave up on its download: it asks "
			"the download client to remove it with its files, and then looks for the next best release (after a "
			"release you chose by hand, you choose the next one too). Nothing in the library was deleted.";
	if (g_strcmp0(done, "cancelled-held") == 0)
		return "Blocked. The release is on the blocklist under your name, and Axis gave up on its download: it asks "
			"the download client to remove it with its files. Axis does not search for another release: nothing is "
			"looked for until you search, or its monitoring says to. Nothing in the library was deleted.";
	if (g_strcmp0(done, "blocked") == 0)
		return "Blocked for the future. The release is on the blocklist under your name, and no search takes it "
			"again. Nothing else changed: no download was cancelled, no file in the library and no link was touched, "
			"and no search was started.";
	return NULL;
}

/* How a closed grab ended, in words. */
static const gchar *
ended_words(const gchar *close_reason)
{
	static const struct
	{
		const gchar *reason;
		const gchar *words;
	} ends[] = {
		{ "imported", "This release is in the library already." },
		{ "failed", "Its download failed." },
		{ "stalled", "Its download stalled, and Axis gave up on it." },
		{ "gone", "Its download was gone from the download client." },
		{ "given_up", "Its download was given up on." },
		{ "removed", "Its download was removed from the download client." },
		{ "dismissed", "Its import was dismissed." },
		{ "deleted", "Its download was deleted from its import's page." },
		{ "replaced", "Its download was replaced with another release from a manual search." },
		{ "not_imported", "Its import took no file of the download." },
		{ "files_gone", "Its files were gone before their import." }
	};
	guint i;

	for (i = 0; close_reason != NULL && i < G_N_ELEMENTS(ends); i++) {
		if (strcmp(ends[i].reason, close_reason) == 0)
			return ends[i].words;
	}
	return "Its download has ended.";
}

static HtmxResponse *
page(Work *work)
{
	const AxisWebBlockInfo *info = work->reg->info;
	const AxisAcquireBlockGrab *grab = work->read.grab;
	g_autoptr(HtmxDiv) content = htmx_div_new();
	HtmxHeading *title = htmx_h1_new();
	HtmxNode *card = axis_web_storage_card("What it does");
	HtmxNode *facts = axis_web_browser_facts_new();
	const gchar *said = done_text(work->done);

	htmx_node_set_text_content(HTMX_NODE(title), grab->open ? (grab->searches ? "Block this release and search again?" :
		"Block this release and give up on it?") : "Block this release?");
	ADD(content, title);
	{
		HtmxP *line = htmx_p_new();

		htmx_element_add_class(HTMX_ELEMENT(line), "series-back");
		ADD(line, axis_web_browser_link(work->read.back, work->read.title));
		ADD(content, line);
	}
	if (said != NULL)
		ADD(content, axis_web_auth_notice(FALSE, said));
	htmx_element_set_id(HTMX_ELEMENT(card), "block-confirm");
	axis_web_browser_fact_node(facts, "Release", axis_web_browser_name_node(grab->release_name));
	axis_web_browser_fact(facts, "Grabbed for", work->read.title);
	if (grab->indexer != NULL)
		axis_web_browser_fact(facts, "Found at", grab->indexer);
	ADD(card, facts);
	if (grab->blocked && !grab->open) {
		/* Nothing to do: on the blocklist, and no download to give up on. */
		HtmxP *line = htmx_p_new();
		HtmxSpan *words = htmx_span_new();
		g_autofree gchar *text = g_strdup_printf("This release is on the blocklist of %s: no search takes it. To "
			"undo that, take it off the blocklist. ", work->read.title);

		htmx_element_add_class(HTMX_ELEMENT(line), "help");
		htmx_node_set_text_content(HTMX_NODE(words), text);
		ADD(line, words);
		ADD(line, axis_web_browser_link(work->read.blocklist, "See the blocklist"));
		ADD(card, line);
		ADD(content, card);
		return axis_web_page_respond(work->request, "Block a release", HTMX_NODE(content));
	}
	if (grab->open) {
		g_autofree gchar *text = g_strdup_printf("Its download is still Axis's: it is downloading, or it has downloaded "
			"and waits for its import. Blocking it puts the release on the blocklist of %s under your name, gives "
			"up on the download (%s)%s Nothing in the library is deleted.", work->read.title, grab->at_client ?
			"Axis asks the download client to remove it with its files" : "nothing of it is at a download client",
			grab->searches ? ", and lets Axis search for the next best release." : ". Axis does not search for another "
			"release afterwards: this release is not monitored, or you chose it by hand, so the next one is yours to "
			"choose.");

		ADD(card, axis_web_storage_text("manual-sentence", text));
	} else {
		g_autofree gchar *text = g_strdup_printf("%s Blocking it only puts the release on the blocklist of %s "
			"under your name, for the future: no search takes it again. No download is cancelled, no file in the "
			"library and no link is touched, and no search is started. To replace a file, search for the %s "
			"yourself afterwards.", ended_words(grab->close_reason), work->read.title, info->noun);

		ADD(card, axis_web_storage_text("manual-sentence", text));
	}
	ADD(card, axis_web_storage_text("help", "A release you block is blocked by its name from every indexer, and by its "
		"hash when it is a torrent. To undo it, take it off the blocklist."));
	{
		g_autofree gchar *action = g_strdup_printf("%s/block-release", info->prefix);
		g_autofree gchar *owner = g_strdup_printf("%" G_GINT64_FORMAT, grab->owner_id);
		g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT, grab->grab_id);
		HtmxNode *form = axis_web_auth_post_form(work->request, action);

		htmx_element_add_class(HTMX_ELEMENT(form), "manual-confirm-form");
		ADD(form, axis_web_auth_hidden("owner", owner));
		ADD(form, axis_web_auth_hidden("grab", id));
		ADD(form, axis_web_storage_checkbox("block-confirm-tick", grab->open ? "Block this release and give up on "
			"its download" : "Block this release for the future", "confirm", TRUE));
		ADD(form, axis_web_auth_button(grab->open ? (grab->searches ? "Block and search again" :
			"Block and give up on it") : "Block this release",
			"button-danger button-small"));
		ADD(card, form);
	}
	ADD(content, card);
	return axis_web_page_respond(work->request, "Block a release", HTMX_NODE(content));
}

/* ── Work off the main loop ─────────────────────────────────────── */

static void
work_thread(
	GTask        *task,
	gpointer      source,
	gpointer      task_data,
	GCancellable *cancellable
){
	Work *work = task_data;
	const AxisWebBlockInfo *info = work->reg->info;

	if (info->find(work->reg->data, work->grab_id, work->download_id, work->import_id, &work->read, &work->error) &&
	    work->post) {
		const AxisAcquireBlockGrab *grab = work->read.grab;

		/* The post names the owner its page showed. */
		if (grab->owner_id != work->owner_id)
			g_set_error_literal(&work->error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "the form does not name this "
				"download");
		else
			info->block(work->reg->data, grab->owner_id, grab->grab_id, work->user, &work->cancelled, &work->error);
	}
	g_task_return_boolean(task, TRUE);
}

static void
work_finished(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	Work *work = user_data;
	HtmxResponse *response;

	if (g_error_matches(work->error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT)) {
		response = axis_web_error_respond(work->request, 400, "Not blocked", "The form does not name this download. "
			"Open the page again.");
	} else if (work->error != NULL) {
		response = failed(work, work->post ? "Not blocked" : "Not shown");
	} else if (work->post) {
		const AxisAcquireBlockGrab *grab = work->read.grab;
		g_autofree gchar *here = axis_web_block_path(work->reg->info->prefix, "grab", grab->grab_id);
		g_autofree gchar *location = g_strdup_printf("%s&done=%s", here, work->cancelled ? (grab->searches ?
			"cancelled" : "cancelled-held") : "blocked");

		axis_web_storage_audit(work->request, "%s %" G_GINT64_FORMAT " grab %" G_GINT64_FORMAT " release put on the "
			"blocklist by hand, %s", work->reg->info->noun, grab->owner_id, grab->grab_id, work->cancelled ?
			"its download given up on and the next best looked for" : "for the future only: nothing else changed");
		response = axis_web_auth_redirect(work->request, location);
	} else {
		response = page(work);
	}
	htmx_response_complete(work->deferred, response);
	g_object_unref(response);
	work->reg->info->release(work->reg->data);
	work_free(work);
}

static gint64
number_of(const gchar *text)
{
	guint64 parsed = 0;

	if (text == NULL || !g_ascii_string_to_unsigned(text, 10, 1, G_MAXINT64, &parsed, NULL))
		return 0;
	return (gint64)parsed;
}

static HtmxResponse *
handle(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	const Registered *reg = user_data;
	gboolean post = htmx_request_get_method(request) == HTMX_METHOD_POST;
	const gchar *done = htmx_request_get_query_param(request, "done");
	HtmxResponse *returned;
	Work *work;
	GTask *task;

	work = g_new0(Work, 1);
	work->reg = reg;
	work->post = post;
	if (post) {
		work->owner_id = number_of(htmx_request_get_form_value(request, "owner"));
		work->grab_id = number_of(htmx_request_get_form_value(request, "grab"));
	} else {
		work->grab_id = number_of(htmx_request_get_query_param(request, "grab"));
		work->download_id = number_of(htmx_request_get_query_param(request, "download"));
		work->import_id = number_of(htmx_request_get_query_param(request, "import"));
		/* Only a known word is kept: nothing from the address is shown. */
		work->done = g_strcmp0(done, "cancelled") == 0 ? "cancelled" : g_strcmp0(done, "cancelled-held") == 0 ?
			"cancelled-held" : g_strcmp0(done, "blocked") == 0 ? "blocked" : NULL;
	}
	if (work->grab_id <= 0 && work->download_id <= 0 && work->import_id <= 0) {
		work_free(work);
		return axis_web_error_respond(request, 400, post ? "Not blocked" : "Not shown", "Say which download.");
	}
	if (post && (work->owner_id <= 0 || !axis_web_storage_form_yes(request, "confirm"))) {
		work_free(work);
		return axis_web_error_respond(request, 400, "Not blocked", "Tick the box to confirm.");
	}
	if (!reg->info->hold(reg->data)) {
		work_free(work);
		returned = axis_web_error_respond(request, 503, "Busy", "The server is busy. Try again in a few seconds.");
		htmx_response_add_header(returned, "Retry-After", "5");
		return returned;
	}
	work->request = g_object_ref(request);
	work->user = g_strdup(axis_web_request_get_username(request));
	work->deferred = htmx_response_new_deferred();
	returned = g_object_ref(work->deferred);
	task = g_task_new(NULL, NULL, work_finished, work);
	g_task_set_task_data(task, work, NULL);
	g_task_run_in_thread(task, work_thread);
	g_object_unref(task);
	return returned;
}

void
axis_web_block_register(
	AxisWeb                *web,
	const AxisWebBlockInfo *info,
	gpointer                data
){
	Registered *reg = g_new0(Registered, 1);
	g_autofree gchar *path = g_strdup_printf("%s/block-release", info->prefix);

	reg->info = info;
	reg->data = data;
	axis_web_keep(web, reg, g_free);
	axis_web_route_full(web, HTMX_METHOD_GET, path, AXIS_WEB_ROUTE_ADMIN, handle, reg);
	axis_web_route_full(web, HTMX_METHOD_POST, path, AXIS_WEB_ROUTE_ADMIN, handle, reg);
}
