/*
 * axis-web-youtube.c - the YouTube module: its pages and its job
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "web/axis-web-youtube.h"

#include <string.h>

#include "core/axis-size.h"
#include "jobs/axis-job-registry.h"
#include "web/axis-web.h"
#include "web/axis-web-attention.h"
#include "web/axis-web-auth-private.h"
#include "web/axis-web-browser-private.h"
#include "web/axis-web-downloads-private.h"
#include "web/axis-web-storage-private.h"
#include "web/axis-web-table.h"
#include "web/axis-web-text.h"
#include "youtube/axis-youtube-address.h"

#define ADD axis_web_storage_add

#define PAGE_PATH    "/youtube"
#define ADMIN_PATH   "/admin/youtube"
#define MAX_PENDING  8

/* Said where YouTube is switched on, and nowhere softened. */
#define NOTICE \
	"Downloading from YouTube may be against YouTube's terms of service, and against the law where you live. " \
	"Axis does not decide that for you: you are responsible for what you download."

struct _AxisWebYoutube
{
	AxisYoutube *youtube;
	AxisStorage *storage;
	guint        pending;	/* dispatched, not yet answered */
	gboolean     stopping;
};

/* ── Work off the main loop ─────────────────────────────────────── */

typedef enum
{
	ACTION_NONE,
	ACTION_LOOK,
	ACTION_DOWNLOAD,
	ACTION_SWITCH,
	ACTION_RECOVER	/* no request: what a killed daemon left, at the start */
} Action;

typedef struct
{
	AxisWebYoutube    *owner;
	HtmxRequest       *request;
	HtmxResponse      *deferred;
	gboolean           admin_page;
	Action             action;
	gchar             *address;
	gchar             *video_id;
	gint64             root_id;
	gboolean           on;
	gchar             *user;
	guint              stopped;	/* downloads the switch stopped */
	/* What was found. */
	AxisYoutubeStatus  status;
	GPtrArray         *videos;
	GPtrArray         *roots;	/* AxisRootInfo, the YouTube ones */
	AxisYoutubeVideo  *video;	/* the one looked at */
	GError            *failure;	/* the action's */
	GError            *read_error;
} Work;

static void
work_free(Work *work)
{
	g_free(work->address);
	g_free(work->video_id);
	g_free(work->user);
	axis_youtube_status_clear(&work->status);
	g_clear_pointer(&work->videos, g_ptr_array_unref);
	g_clear_pointer(&work->roots, g_ptr_array_unref);
	g_clear_pointer(&work->video, axis_youtube_video_free);
	g_clear_error(&work->failure);
	g_clear_error(&work->read_error);
	g_clear_object(&work->request);
	g_clear_object(&work->deferred);
	g_free(work);
}

static void
work_run(Work *work)
{
	AxisYoutube *youtube = work->owner->youtube;
	g_autoptr(GPtrArray) roots = NULL;
	guint i;

	switch (work->action) {
	case ACTION_LOOK:
		axis_youtube_look(youtube, work->address, work->user, &work->video, &work->failure);
		break;
	case ACTION_DOWNLOAD:
		axis_youtube_download(youtube, work->video_id, work->root_id, work->user, NULL, &work->failure);
		break;
	case ACTION_SWITCH:
		axis_youtube_set_enabled(youtube, work->on, work->user, &work->stopped, &work->failure);
		break;
	case ACTION_RECOVER:
		axis_youtube_recover(youtube);
		return;
	default:
		break;
	}
	/* The page, as it is after that.  yt-dlp is asked its version for the
	 * administrator's page only: the page everyone signed in can open
	 * starts no program, and shows what is known. */
	if (!axis_youtube_get_status(youtube, work->admin_page, &work->status, &work->read_error))
		return;
	if (work->admin_page)
		return;
	if (!axis_youtube_list_videos(youtube, 200, &work->videos, &work->read_error) ||
	    !axis_storage_list_roots(work->owner->storage, 0, &roots, &work->read_error))
		return;
	work->roots = g_ptr_array_new_with_free_func((GDestroyNotify)axis_root_info_free);
	for (i = roots->len; i > 0; i--) {
		AxisRootInfo *root = g_ptr_array_index(roots, i - 1);

		if (root->media_type == AXIS_MEDIA_YOUTUBE)
			g_ptr_array_insert(work->roots, 0, g_ptr_array_steal_index(roots, i - 1));
	}
	if (work->video == NULL && work->video_id != NULL)
		axis_youtube_get_video(youtube, work->video_id, &work->video, NULL);
}

static void
work_thread(
	GTask        *task,
	gpointer      source,
	gpointer      task_data,
	GCancellable *cancellable
){
	work_run(task_data);
	g_task_return_boolean(task, TRUE);
}

static HtmxResponse *answer(Work *work);

static void
work_finished(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	Work *work = user_data;
	HtmxResponse *response;

	work->owner->pending--;
	if (work->action == ACTION_RECOVER) {
		work_free(work);
		return;
	}
	response = answer(work);
	htmx_response_complete(work->deferred, response);
	g_object_unref(response);
	work_free(work);
}

/* Runs @work on a worker thread and answers its request afterwards; a 503
 * at once when too much is waiting or the pages are stopping. */
static HtmxResponse *
dispatch(
	AxisWebYoutube *self,
	HtmxRequest    *request,
	Work           *work
){
	HtmxResponse *returned;
	GTask *task;

	if (self->stopping || self->pending >= MAX_PENDING) {
		work_free(work);
		returned = axis_web_error_respond(request, 503, "Busy", "The server is busy. Try again in a few seconds.");
		htmx_response_add_header(returned, "Retry-After", "5");
		return returned;
	}
	work->owner = self;
	work->request = g_object_ref(request);
	work->deferred = htmx_response_new_deferred();
	work->user = g_strdup(axis_web_request_get_username(request));
	returned = g_object_ref(work->deferred);
	self->pending++;
	task = g_task_new(NULL, NULL, work_finished, work);
	g_task_set_task_data(task, work, NULL);
	g_task_run_in_thread(task, work_thread);
	g_object_unref(task);
	return returned;
}

/* ── Pieces ─────────────────────────────────────────────────────── */

static gchar *
length_text(gint64 seconds)
{
	if (seconds < 0)
		return g_strdup("Not known");
	if (seconds >= 3600)
		return g_strdup_printf("%" G_GINT64_FORMAT " h %02" G_GINT64_FORMAT " min", seconds / 3600,
			(seconds % 3600) / 60);
	return g_strdup_printf("%" G_GINT64_FORMAT " min %02" G_GINT64_FORMAT " s", seconds / 60, seconds % 60);
}

static const gchar *
state_badge(AxisYoutubeVideoState state)
{
	switch (state) {
	case AXIS_YOUTUBE_FETCHED:
		return "badge-succeeded";
	case AXIS_YOUTUBE_FAILED:
		return "badge-failed";
	case AXIS_YOUTUBE_CANCELLED:
		return "badge-warn";
	default:
		return "badge-note";
	}
}

/* Why a video is where it is, or how far it has come. */
static gchar *
video_words(const AxisYoutubeVideo *video)
{
	switch (video->state) {
	case AXIS_YOUTUBE_FETCHED:
		return g_strdup_printf("%s: %s", video->volume_name != NULL ? video->volume_name : "A disk",
			video->rel_path != NULL ? video->rel_path : "");
	case AXIS_YOUTUBE_FAILED:
	case AXIS_YOUTUBE_CANCELLED:
		return g_strdup(video->message != NULL ? video->message : "It was stopped.");
	case AXIS_YOUTUBE_FETCHING: {
		gint64 whole = video->size_bytes > 0 ? video->size_bytes : video->expected_bytes;

		if (video->done_bytes >= 0 && whole > 0)
			return g_strdup_printf("%.0f%% downloaded", 100.0 * (gdouble)MIN(video->done_bytes, whole) / (gdouble)whole);
		return g_strdup("Downloading");
	}
	case AXIS_YOUTUBE_WANTED:
		return g_strdup("Waiting for its turn: one video is downloaded at a time.");
	default:
		return g_strdup("Looked up, not downloaded.");
	}
}

static HtmxNode *
video_table(GPtrArray *videos)
{
	static const AxisWebColumn columns[] = {
		{ "Video", AXIS_WEB_COL_NAME },
		{ "Channel", AXIS_WEB_COL_TEXT },
		{ "Uploaded", AXIS_WEB_COL_DATE },
		{ "State", AXIS_WEB_COL_STATE },
		{ "Size", AXIS_WEB_COL_SIZE },
		{ "Where it is", AXIS_WEB_COL_WIDE },
		{ NULL, 0 }
	};
	HtmxNode *table = axis_web_table_new("youtube-videos", columns);
	HtmxTbody *body = htmx_tbody_new();
	guint i;

	for (i = 0; i < videos->len; i++) {
		const AxisYoutubeVideo *video = g_ptr_array_index(videos, i);
		HtmxTr *row = htmx_tr_new();
		HtmxTd *name = htmx_td_new();
		HtmxTd *channel = htmx_td_new();
		HtmxTd *state = htmx_td_new();
		HtmxTd *where = htmx_td_new();
		HtmxBdi *title = htmx_bdi_new();
		HtmxBdi *channel_name = htmx_bdi_new();
		HtmxBdi *words_node = htmx_bdi_new();
		gint64 bytes = video->size_bytes > 0 ? video->size_bytes : video->expected_bytes;
		g_autofree gchar *size = bytes > 0 ? axis_size_format((guint64)bytes) : g_strdup("");
		g_autofree gchar *words = video_words(video);

		ADD(title, axis_web_breakable(video->title));
		ADD(name, title);
		ADD(row, name);
		htmx_node_set_text_content(HTMX_NODE(channel_name), video->channel);
		ADD(channel, channel_name);
		ADD(row, channel);
		ADD(row, htmx_td_new_with_text(video->upload_date != NULL ? video->upload_date : ""));
		ADD(state, axis_web_browser_badge(axis_youtube_video_state_label(video->state), state_badge(video->state)));
		ADD(row, state);
		ADD(row, htmx_td_new_with_text(size));
		ADD(words_node, axis_web_breakable(words));
		ADD(where, words_node);
		ADD(row, where);
		ADD(body, row);
	}
	ADD(table, body);
	return table;
}

/* What was looked up, and the button that downloads it. */
static HtmxNode *
looked_card(
	HtmxRequest            *request,
	const AxisYoutubeVideo *video,
	GPtrArray              *roots
){
	HtmxNode *card = axis_web_storage_card("This video");
	HtmxNode *facts = axis_web_browser_facts_new();
	g_autofree gchar *length = length_text(video->duration_s);
	g_autofree gchar *size = video->expected_bytes > 0 ? axis_size_format((guint64)video->expected_bytes) : NULL;
	g_autofree gchar *about = size != NULL ? g_strdup_printf("About %s", size) : g_strdup("Not known until it is "
		"downloaded");
	guint i;

	axis_web_browser_fact_node(facts, "Title", axis_web_browser_name_node(video->title));
	axis_web_browser_fact_node(facts, "Channel", axis_web_browser_name_node(video->channel));
	axis_web_browser_fact(facts, "Uploaded", video->upload_date != NULL ? video->upload_date : "Not known");
	axis_web_browser_fact(facts, "Length", length);
	axis_web_browser_fact(facts, "What is taken", video->format_note != NULL ? video->format_note :
		"The best picture and sound within the limit");
	axis_web_browser_fact(facts, "Size", about);
	ADD(card, facts);
	if (video->state == AXIS_YOUTUBE_WANTED || video->state == AXIS_YOUTUBE_FETCHING) {
		ADD(card, axis_web_storage_text("help", "It is being downloaded. The Downloads page shows how far it is."));
		return card;
	}
	if (video->state == AXIS_YOUTUBE_FETCHED) {
		g_autofree gchar *where = video_words(video);
		g_autofree gchar *text = g_strdup_printf("It is in the library already (%s). It is downloaded again only when "
			"that file is gone.", where);

		ADD(card, axis_web_storage_text("help", text));
	}
	if (roots == NULL || roots->len == 0) {
		ADD(card, axis_web_storage_text("help", "There is no YouTube library folder yet. Add one on the Storage page: "
			"a library folder of the kind YouTube. Then come back and look the video up again."));
		return card;
	}
	ADD(card, axis_web_storage_text("help", "It is saved as YouTube serves it, without re-encoding, under its "
		"channel and the year it was uploaded."));
	for (i = 0; i < roots->len; i++) {
		const AxisRootInfo *root = g_ptr_array_index(roots, i);
		HtmxNode *form = axis_web_auth_post_form(request, PAGE_PATH "/download");
		g_autofree gchar *root_text = g_strdup_printf("%" G_GINT64_FORMAT, root->id);
		g_autofree gchar *label = roots->len == 1 ? g_strdup("Download") : g_strdup_printf("Download to %s", root->name);

		htmx_element_add_class(HTMX_ELEMENT(form), "form");
		ADD(form, axis_web_auth_hidden("video", video->video_id));
		ADD(form, axis_web_auth_hidden("root", root_text));
		ADD(form, axis_web_auth_button(label, NULL));
		ADD(card, form);
	}
	return card;
}

static guint
failure_status(const GError *failure)
{
	if (failure->domain != AXIS_YOUTUBE_ERROR)
		return 409;
	switch (failure->code) {
	case AXIS_YOUTUBE_ERROR_INVALID:
		return 400;
	case AXIS_YOUTUBE_ERROR_NOT_FOUND:
		return 404;
	case AXIS_YOUTUBE_ERROR_BUSY:
		return 503;
	default:
		return 409;
	}
}

/* Whether @failure is one to show as it is (the layers' own words, for a
 * person), not a fault of the server. */
static gboolean
failure_is_said(const GError *failure)
{
	return failure->domain == AXIS_YOUTUBE_ERROR || failure->domain == AXIS_DOWNLOAD_ERROR;
}

/* ── Media, then YouTube ────────────────────────────────────────── */

static HtmxResponse *
page(Work *work)
{
	g_autoptr(HtmxDiv) content = htmx_div_new();
	HtmxHeading *title = htmx_h1_new();
	HtmxRequest *request = work->request;
	gboolean admin = axis_web_request_is_admin(request);
	const gchar *done = htmx_request_get_query_param(request, "done");
	HtmxResponse *response;
	HtmxNode *card;

	htmx_node_set_text_content(HTMX_NODE(title), "YouTube");
	ADD(content, title);
	ADD(content, axis_web_page_intro("Videos downloaded from YouTube into a library folder of their own, each under "
		"its channel. Paste a video's address, look at what it is, and download it."));
	if (work->failure != NULL)
		ADD(content, axis_web_auth_notice(TRUE, work->failure->message));
	else if (g_strcmp0(done, "queued") == 0)
		ADD(content, axis_web_auth_notice(FALSE, "The download is queued. It is listed below and on the Downloads "
			"page, where its progress shows."));
	if (work->status.blocked != NULL) {
		HtmxP *line = htmx_p_new();

		htmx_element_add_class(HTMX_ELEMENT(line), "help");
		htmx_node_set_text_content(HTMX_NODE(line), work->status.blocked);
		ADD(content, line);
		if (admin)
			ADD(content, axis_web_browser_link(ADMIN_PATH, "YouTube downloader"));
	}

	if (admin && work->status.blocked == NULL) {
		HtmxNode *form = axis_web_auth_post_form(request, PAGE_PATH "/look");

		card = axis_web_storage_card("Download a video");
		htmx_element_add_class(HTMX_ELEMENT(form), "form");
		ADD(form, axis_web_storage_text_field("youtube-address", "Address of the video", "address",
			work->failure != NULL && work->address != NULL ? work->address : "", "https://www.youtube.com/watch?v=...",
			"One video at a time. Axis first shows what the video is; nothing is downloaded until you say so."));
		ADD(form, axis_web_auth_button("Look it up", NULL));
		ADD(card, form);
		ADD(content, card);
	}
	if (admin && work->video != NULL && work->failure == NULL)
		ADD(content, looked_card(request, work->video, work->roots));

	card = axis_web_storage_card("Videos");
	if (work->videos == NULL || work->videos->len == 0)
		ADD(card, axis_web_storage_text("help", "No video yet."));
	else
		ADD(card, video_table(work->videos));
	ADD(content, card);
	ADD(content, axis_web_browser_link("/downloads", "Downloads"));

	response = axis_web_page_respond(request, "YouTube", HTMX_NODE(content));
	if (work->failure != NULL)
		htmx_response_set_status(response, failure_status(work->failure));
	return response;
}

/* ── System, then YouTube downloader ────────────────────────────── */

static HtmxResponse *
admin_page(Work *work)
{
	g_autoptr(HtmxDiv) content = htmx_div_new();
	HtmxHeading *title = htmx_h1_new();
	HtmxRequest *request = work->request;
	const AxisYoutubeStatus *status = &work->status;
	HtmxNode *card = axis_web_storage_card(status->enabled ? "YouTube is switched on" : "YouTube is switched off");
	HtmxNode *facts = axis_web_browser_facts_new();
	HtmxNode *form = axis_web_auth_post_form(request, ADMIN_PATH "/switch");
	const gchar *stopped = htmx_request_get_query_param(request, "stopped");
	HtmxResponse *response;

	htmx_node_set_text_content(HTMX_NODE(title), "YouTube downloader");
	ADD(content, title);
	ADD(content, axis_web_page_intro("Whether Axis downloads from YouTube at all, and the yt-dlp program it does it "
		"with. Nothing is asked of YouTube until this is switched on."));
	if (work->failure != NULL) {
		ADD(content, axis_web_auth_notice(TRUE, work->failure->message));
	} else if (!status->enabled && stopped != NULL && stopped[0] != '\0' && strspn(stopped, "0123456789") ==
	           strlen(stopped) && strlen(stopped) <= 4) {
		g_autofree gchar *text = strcmp(stopped, "1") == 0 ?
			g_strdup("YouTube is switched off. The download that was running was stopped, and what it had written "
				"was removed; it can be asked for again once YouTube is switched on.") :
			g_strdup_printf("YouTube is switched off. %s downloads that were running or waiting were stopped, and what "
				"they had written was removed; they can be asked for again once YouTube is switched on.", stopped);

		ADD(content, axis_web_auth_notice(FALSE, text));
	}

	ADD(card, axis_web_auth_notice(FALSE, NOTICE));
	htmx_element_add_class(HTMX_ELEMENT(form), "form");
	if (status->enabled) {
		g_autofree gchar *when = axis_web_format_time(status->enabled_us, "an unknown time");
		g_autofree gchar *who = g_strdup_printf("Switched on by %s, %s.", status->enabled_by != NULL ?
			status->enabled_by : "an administrator", when);

		ADD(card, axis_web_storage_text(NULL, who));
		ADD(card, axis_web_storage_text("help", "Switching YouTube off stops the download that is running and those "
			"that wait, and removes what they had written so far. Videos already in the library stay."));
		ADD(form, axis_web_auth_hidden("on", "no"));
		ADD(form, axis_web_auth_button("Switch YouTube off", "button-danger"));
	} else {
		ADD(form, axis_web_auth_hidden("on", "yes"));
		ADD(form, axis_web_storage_checkbox("youtube-understood", "I have read this, and what I download is mine to "
			"answer for", "understood", TRUE));
		ADD(form, axis_web_auth_button("Switch YouTube on", NULL));
	}
	ADD(card, form);
	ADD(content, card);

	card = axis_web_storage_card("yt-dlp");
	axis_web_browser_fact(facts, "Program", status->program != NULL ? status->program :
		"Not found where the setting says (youtube.ytdlp_path)");
	if (status->version != NULL) {
		g_autofree gchar *version = g_strdup_printf("%s (%s)", status->version, status->version_from_build ?
			"as recorded when the image was built; yt-dlp itself is not run while YouTube is off" :
			"as the program says");

		axis_web_browser_fact(facts, "Version", version);
	} else {
		axis_web_browser_fact(facts, "Version", status->enabled ? "The program did not say" :
			"Not asked while YouTube is switched off");
	}
	axis_web_browser_fact(facts, "Outbound binding", status->bound ?
		"Set (outbound.bind): YouTube work needs a proxy for YouTube, or the confirmation that all of Axis's network "
		"goes through the VPN" : "Not set: yt-dlp follows the system's routes");
	if (status->paused_until_us > 0) {
		g_autofree gchar *until = axis_web_format_time(status->paused_until_us, "later");

		axis_web_browser_fact(facts, "Paused until", until);
	}
	ADD(card, facts);
	if (status->enabled && status->blocked != NULL)
		ADD(card, axis_web_auth_notice(TRUE, status->blocked));
	ADD(card, axis_web_storage_text("help", "yt-dlp is a separate program that Axis runs with a fixed set of options. "
		"The image takes the newest yt-dlp when it is built, and Axis never updates it by itself. YouTube changes "
		"often: when downloads start to fail, build or pull a newer image, or point the setting \"yt-dlp program\" at "
		"a newer copy."));
	ADD(card, axis_web_browser_link("/settings", "Settings"));
	ADD(content, card);
	ADD(content, axis_web_browser_link(PAGE_PATH, "YouTube"));

	response = axis_web_page_respond(request, "YouTube downloader", HTMX_NODE(content));
	if (work->failure != NULL)
		htmx_response_set_status(response, failure_status(work->failure));
	return response;
}

static HtmxResponse *
answer(Work *work)
{
	HtmxRequest *request = work->request;

	if (work->read_error != NULL)
		return axis_web_internal_error(request, work->read_error);
	if (work->failure != NULL && !failure_is_said(work->failure))
		return axis_web_internal_error(request, work->failure);
	if (work->failure == NULL) {
		switch (work->action) {
		case ACTION_LOOK: {
			g_autofree gchar *where = g_strdup_printf(PAGE_PATH "?video=%s", work->video->video_id);

			axis_web_storage_audit(request, "YouTube video %s looked up", work->video->video_id);
			return axis_web_auth_redirect(request, where);
		}
		case ACTION_DOWNLOAD:
			axis_web_storage_audit(request, "YouTube video %s queued for download", work->video_id);
			return axis_web_auth_redirect(request, PAGE_PATH "?done=queued");
		case ACTION_SWITCH: {
			g_autofree gchar *where = work->stopped > 0 ? g_strdup_printf(ADMIN_PATH "?stopped=%u", work->stopped) :
				g_strdup(ADMIN_PATH);

			axis_web_storage_audit(request, "YouTube switched %s", work->on ? "on" : "off");
			return axis_web_auth_redirect(request, where);
		}
		default:
			break;
		}
	}
	return work->admin_page ? admin_page(work) : page(work);
}

/* ── Routes ─────────────────────────────────────────────────────── */

static HtmxResponse *
handle_page(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	Work *work = g_new0(Work, 1);
	const gchar *video = htmx_request_get_query_param(request, "video");

	/* The video just looked up, for the one who may download it. */
	if (axis_web_request_is_admin(request) && axis_youtube_video_id_valid(video))
		work->video_id = g_strdup(video);
	return dispatch(user_data, request, work);
}

static HtmxResponse *
handle_look(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	Work *work = g_new0(Work, 1);
	const gchar *address = htmx_request_get_form_value(request, "address");

	work->action = ACTION_LOOK;
	/* Kept only to show again beside a refusal, and only when it is short
	 * and plain; what yt-dlp gets is never this text. */
	work->address = address != NULL && strlen(address) <= AXIS_YOUTUBE_ADDRESS_MAX && g_utf8_validate(address, -1, NULL) ?
		g_strdup(address) : g_strdup("");
	return dispatch(user_data, request, work);
}

static HtmxResponse *
handle_download(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	const gchar *video = htmx_request_get_form_value(request, "video");
	guint64 root = 0;
	Work *work;

	if (!axis_youtube_video_id_valid(video))
		return axis_web_error_respond(request, 400, "No such video", "Look the video up first, then download it.");
	if (!axis_web_storage_form_u64(request, "root", &root) || root > G_MAXINT64)
		root = 0;
	work = g_new0(Work, 1);
	work->action = ACTION_DOWNLOAD;
	work->video_id = g_strdup(video);
	work->root_id = (gint64)root;
	return dispatch(user_data, request, work);
}

static HtmxResponse *
handle_admin(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	Work *work = g_new0(Work, 1);

	work->admin_page = TRUE;
	return dispatch(user_data, request, work);
}

static HtmxResponse *
handle_switch(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	gboolean on = g_strcmp0(htmx_request_get_form_value(request, "on"), "yes") == 0;
	Work *work;

	if (on && !axis_web_storage_form_yes(request, "understood"))
		return axis_web_error_respond(request, 400, "Not confirmed", "Tick the box to switch YouTube on.");
	work = g_new0(Work, 1);
	work->admin_page = TRUE;
	work->action = ACTION_SWITCH;
	work->on = on;
	return dispatch(user_data, request, work);
}

/*
 * The YouTube videos whose download failed (add-attention-notices, slice N2):
 * Axis does not try one again by itself, a person asks again on the YouTube
 * page, so it is a notice at once; nothing else waits on it, so it is "Worth
 * a look".  One statement over the partial index youtube_video_failed
 * (migration 0077).  "newest" is the second the latest of them failed.  The
 * sentence names no video.
 */
static gboolean
attention_source(
	const AxisAttentionReading  *reading,
	GPtrArray                   *items,
	gpointer                     user_data,
	GError                     **error
){
	AxisWebYoutube *self = user_data;
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *text = NULL;
	AxisAttentionItem *item;
	gint64 n = 0;
	gint64 newest = 0;

	if (!axis_db_pool_query_bounded(axis_storage_get_pool(self->storage),
	        "SELECT count(*), coalesce(floor(extract(epoch FROM max(finished_at)))::bigint, 0) FROM youtube_video"
	        " WHERE state = 'failed'", reading->deadline_ms, &result, error))
		return FALSE;
	axis_db_result_get_int64(result, 0, 0, &n);
	axis_db_result_get_int64(result, 0, 1, &newest);
	if (n <= 0)
		return TRUE;
	text = n == 1 ? g_strdup("The download of 1 YouTube video failed. Axis does not try it again by itself.") :
		g_strdup_printf("The downloads of %" G_GINT64_FORMAT " YouTube videos failed. Axis does not try them again by "
			"itself.", n);
	item = axis_attention_item_new("youtube.failed", "", AXIS_ATTENTION_LOOK, (guint64)n, newest);
	axis_attention_item_say(item, text, NULL, NULL);
	axis_attention_item_link(item, PAGE_PATH, "Open YouTube");
	g_ptr_array_add(items, item);
	return TRUE;
}

static void
register_routes(
	AxisWeb  *web,
	gpointer  user_data
){
	axis_web_add_attention_source(web, "the YouTube downloads", attention_source, user_data);
	axis_web_route_full(web, HTMX_METHOD_GET, PAGE_PATH, AXIS_WEB_ROUTE_DEFAULT, handle_page, user_data);
	axis_web_route_full(web, HTMX_METHOD_POST, PAGE_PATH "/look", AXIS_WEB_ROUTE_ADMIN, handle_look, user_data);
	axis_web_route_full(web, HTMX_METHOD_POST, PAGE_PATH "/download", AXIS_WEB_ROUTE_ADMIN, handle_download, user_data);
	axis_web_route_full(web, HTMX_METHOD_GET, ADMIN_PATH, AXIS_WEB_ROUTE_ADMIN, handle_admin, user_data);
	axis_web_route_full(web, HTMX_METHOD_POST, ADMIN_PATH "/switch", AXIS_WEB_ROUTE_ADMIN, handle_switch, user_data);
}

/* ── The module ─────────────────────────────────────────────────── */

static gboolean
fetch_handler(
	AxisJobContext  *context,
	gpointer         user_data,
	GError         **error
){
	AxisWebYoutube *self = user_data;

	return axis_youtube_run_fetch(self->youtube, context, error);
}

static const AxisJobHandlerInfo handlers[] = {
	/* One attempt: a download that failed is asked for again by a person,
	 * never by itself. */
	{ AXIS_YOUTUBE_FETCH_KIND, AXIS_YOUTUBE_QUEUE, 1, fetch_handler, "Downloading a YouTube video", AXIS_JOB_CLASS_WORK },
	{ NULL, NULL, 0, NULL, NULL, AXIS_JOB_CLASS_UNSET }
};

static const AxisNavEntry nav_entries[] = {
	{ "YouTube", PAGE_PATH, 40, AXIS_NAV_GROUP_MEDIA, FALSE, NULL, 0 },
	{ "YouTube downloader", ADMIN_PATH, 37, AXIS_NAV_GROUP_SYSTEM, TRUE, NULL, 0 },
	{ NULL, NULL, 0, 0, FALSE, NULL, 0 }
};

static const gchar *const depends_on[] = { AXIS_DOWNLOAD_MODULE_ID, NULL };

static const gchar *const retry_elsewhere[] = {
	AXIS_YOUTUBE_FETCH_KIND, "Look the video up again on the YouTube page and download it from there.",
	NULL
};

const AxisModuleInfo *
axis_web_youtube_get_module(void)
{
	static AxisModuleInfo info;

	if (info.id == NULL) {
		info.id = AXIS_YOUTUBE_MODULE_ID;
		info.name = "YouTube";
		info.job_handlers = handlers;
		info.register_routes = register_routes;
		info.nav_entries = nav_entries;
		info.description = "Downloads one YouTube video at a time with yt-dlp, a separate program, into a YouTube "
			"library folder, as YouTube serves it. Off until an administrator switches it on under System, YouTube "
			"downloader.";
		info.settings = axis_youtube_get_setting_table();
		info.depends_on = depends_on;
		info.job_retry_elsewhere = retry_elsewhere;
	}
	return &info;
}

AxisWebYoutube *
axis_web_youtube_new(
	AxisYoutube *youtube,
	AxisStorage *storage
){
	AxisWebYoutube *self;

	g_return_val_if_fail(youtube != NULL && storage != NULL, NULL);

	self = g_new0(AxisWebYoutube, 1);
	self->youtube = youtube;
	self->storage = storage;
	return self;
}

void
axis_web_youtube_recover(AxisWebYoutube *self)
{
	Work *work;
	GTask *task;

	g_return_if_fail(self != NULL);

	if (self->stopping)
		return;
	work = g_new0(Work, 1);
	work->owner = self;
	work->action = ACTION_RECOVER;
	self->pending++;
	task = g_task_new(NULL, NULL, work_finished, work);
	g_task_set_task_data(task, work, NULL);
	g_task_run_in_thread(task, work_thread);
	g_object_unref(task);
}

void
axis_web_youtube_stop(AxisWebYoutube *self)
{
	g_return_if_fail(self != NULL);

	self->stopping = TRUE;
	/* A look in flight and a fetch that runs are cancelled (yt-dlp is
	 * killed): nothing here waits out a time limit. */
	axis_youtube_stop(self->youtube);
	while (self->pending > 0)
		g_main_context_iteration(NULL, TRUE);
}

void
axis_web_youtube_free(AxisWebYoutube *self)
{
	if (self == NULL)
		return;
	axis_web_youtube_stop(self);
	g_free(self);
}
