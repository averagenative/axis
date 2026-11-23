/*
 * axis-web-probe.c - the media probe module: its jobs, settings and pages
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Text that came from a file (tags, titles, languages, errors) was cut and
 * cleaned when it was stored and is escaped again here, as text content.
 * A file's own name is shown only through axis_library_name_display(),
 * inside <bdi>.  Links carry numbers (the file's id) and nothing else.
 */

#include "web/axis-web-probe.h"
#include "web/axis-web-savings.h"
#include "web/axis-web-storage-private.h"

#include <string.h>

#include "jobs/axis-job-builtin.h"
#include "jobs/axis-job-context.h"
#include "jobs/axis-job-registry.h"
#include "library/axis-library.h"
#include "library/axis-library-kind.h"
#include "library/axis-library-name.h"
#include "web/axis-web-notice.h"
#include "web/axis-web-place.h"

#define ADD axis_web_storage_add

/* Page work waiting for a thread beyond this is answered 503. */
#define MAX_PENDING 16

struct _AxisWebProbe
{
	AxisProbe     *probe;	/* owned reference */
	AxisStorage   *storage;	/* owned reference */
	AxisEstimator *estimator;	/* owned reference, nullable: the "If converted" card */
	guint          pending;	/* dispatched, not yet answered */
	gboolean       stopping;
	/* Another module's card (nullable). */
	AxisWebFileCardCollect card_collect;
	AxisWebFileCardRender  card_render;
	GDestroyNotify         card_free;
	gpointer               card_data;
};

/* ── Work off the main loop ─────────────────────────────────────── */

typedef void (*WorkFunc)(AxisWebProbe *self, gpointer data);
typedef HtmxResponse *(*DoneFunc)(AxisWebProbe *self, HtmxRequest *request, gpointer data);

typedef struct
{
	AxisWebProbe   *owner;
	WorkFunc        work;
	DoneFunc        done;
	HtmxRequest    *request;
	HtmxResponse   *deferred;
	gpointer        data;
	GDestroyNotify  free_data;
} Task;

static void
task_thread(
	GTask        *gtask,
	gpointer      source,
	gpointer      task_data,
	GCancellable *cancellable
){
	Task *task = task_data;

	task->work(task->owner, task->data);
	g_task_return_boolean(gtask, TRUE);
}

static void
task_finished(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	Task *task = user_data;
	HtmxResponse *response;

	task->owner->pending--;
	response = task->done(task->owner, task->request, task->data);
	htmx_response_complete(task->deferred, response);
	g_object_unref(response);

	if (task->free_data != NULL)
		task->free_data(task->data);
	g_object_unref(task->request);
	g_object_unref(task->deferred);
	g_free(task);
}

static HtmxResponse *
dispatch(
	AxisWebProbe   *self,
	HtmxRequest    *request,
	WorkFunc        work,
	DoneFunc        done,
	gpointer        data,
	GDestroyNotify  free_data
){
	HtmxResponse *returned;
	GTask *gtask;
	Task *task;

	if (self->stopping || self->pending >= MAX_PENDING) {
		HtmxResponse *busy = axis_web_error_respond(request, 503, "Busy",
			"The server is busy. Try again in a few seconds.");

		if (free_data != NULL)
			free_data(data);
		htmx_response_add_header(busy, "Retry-After", "5");
		return busy;
	}

	task = g_new0(Task, 1);
	task->owner = self;
	task->work = work;
	task->done = done;
	task->request = g_object_ref(request);
	task->deferred = htmx_response_new_deferred();
	task->data = data;
	task->free_data = free_data;
	returned = g_object_ref(task->deferred);

	self->pending++;
	gtask = g_task_new(NULL, NULL, task_finished, task);
	g_task_set_task_data(gtask, task, NULL);
	g_task_run_in_thread(gtask, task_thread);
	g_object_unref(gtask);

	return returned;
}

/* ── Pieces ─────────────────────────────────────────────────────── */

static HtmxResponse *
not_found(HtmxRequest *request)
{
	return axis_web_error_respond(request, 404, "Not found", "There is nothing at this address.");
}

static HtmxNode *
link_to(
	const gchar *href,
	const gchar *text
){
	HtmxA *link = htmx_a_new_with_href(href);

	htmx_node_set_text_content(HTMX_NODE(link), text);
	return HTMX_NODE(link);
}

static HtmxNode *
facts_new(void)
{
	HtmxDl *list = htmx_dl_new();

	htmx_element_add_class(HTMX_ELEMENT(list), "facts");
	return HTMX_NODE(list);
}

static void
fact(
	HtmxNode    *list,
	const gchar *label,
	const gchar *text
){
	ADD(list, htmx_dt_new_with_text(label));
	ADD(list, htmx_dd_new_with_text(text != NULL ? text : ""));
}

static void
fact_node(
	HtmxNode    *list,
	const gchar *label,
	HtmxNode    *value
){
	HtmxDd *dd = htmx_dd_new();

	ADD(list, htmx_dt_new_with_text(label));
	ADD(dd, value);
	ADD(list, dd);
}

static HtmxNode *
badge(
	const gchar *text,
	const gchar *css
){
	HtmxSpan *span = htmx_span_new();

	htmx_element_add_class(HTMX_ELEMENT(span), "badge");
	if (css != NULL)
		htmx_element_add_class(HTMX_ELEMENT(span), css);
	htmx_node_set_text_content(HTMX_NODE(span), text);
	return HTMX_NODE(span);
}

static HtmxNode *
warning(const gchar *text)
{
	HtmxP *notice = htmx_p_new();

	htmx_element_add_class(HTMX_ELEMENT(notice), "notice");
	htmx_element_add_class(HTMX_ELEMENT(notice), "notice-warning");
	htmx_element_set_attribute(HTMX_ELEMENT(notice), "role", "alert");
	htmx_node_set_text_content(HTMX_NODE(notice), text);
	return HTMX_NODE(notice);
}

/* Text from a file (or its name), isolated from what is around it. */
static HtmxNode *
isolated(const gchar *text)
{
	HtmxBdi *bdi = htmx_bdi_new();

	htmx_node_set_text_content(HTMX_NODE(bdi), text != NULL ? text : "");
	return HTMX_NODE(bdi);
}

/* A stored path as text a person can read. */
static HtmxNode *
path_node(const gchar *stored)
{
	HtmxCode *code = htmx_code_new();
	g_autofree gchar *shown = axis_library_name_display(stored);

	htmx_element_add_class(HTMX_ELEMENT(code), "scan-path");
	ADD(code, isolated(shown));
	return HTMX_NODE(code);
}

static void
cell(
	HtmxNode    *row,
	const gchar *text
){
	ADD(row, htmx_td_new_with_text(text != NULL ? text : ""));
}

/* A cell holding text from a file ("-" for none). */
static void
cell_isolated(
	HtmxNode    *row,
	const gchar *text
){
	HtmxTd *td = htmx_td_new();

	if (text == NULL || text[0] == '\0')
		htmx_node_set_text_content(HTMX_NODE(td), "-");
	else
		ADD(td, isolated(text));
	ADD(row, td);
}

static void
cell_node(
	HtmxNode *row,
	HtmxNode *node
){
	HtmxTd *td = htmx_td_new();

	ADD(td, node);
	ADD(row, td);
}

/* ── Formatting ─────────────────────────────────────────────────── */

static gchar *
format_count(guint64 value)
{
	return g_strdup_printf("%" G_GUINT64_FORMAT, value);
}

static gchar *
format_duration(gint64 ms)
{
	gint64 seconds;

	if (ms < 0)
		return g_strdup("unknown");
	seconds = ms / 1000;
	if (seconds >= 3600)
		return g_strdup_printf("%" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT,
			seconds / 3600, (seconds % 3600) / 60, seconds % 60);
	return g_strdup_printf("%" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT, seconds / 60, seconds % 60);
}

/* "8.2 Mb/s", "about 8.2 Mb/s" for an estimate, "unknown". */
static gchar *
format_bitrate(
	gint64   bits,
	gboolean estimated
){
	g_autofree gchar *value = NULL;

	if (bits <= 0)
		return g_strdup("unknown");
	if (bits >= 1000000)
		value = g_strdup_printf("%.1f Mb/s", (gdouble)bits / 1000000.0);
	else
		value = g_strdup_printf("%" G_GINT64_FORMAT " kb/s", bits / 1000);
	return estimated ? g_strdup_printf("about %s (estimated)", value) : g_steal_pointer(&value);
}

static gchar *
format_size(
	gint     width,
	gint     height
){
	if (width <= 0 || height <= 0)
		return g_strdup("unknown");
	return g_strdup_printf("%d x %d", width, height);
}

static gchar *
format_rate(gdouble rate)
{
	if (rate <= 0)
		return g_strdup("unknown");
	return g_strdup_printf("%.3f fps", rate);
}

static const gchar *
tri_text(
	gint         value,
	const gchar *yes,
	const gchar *no
){
	if (value == AXIS_TRI_YES)
		return yes;
	if (value == AXIS_TRI_NO)
		return no;
	return "unknown";
}

static const gchar *
status_label(const gchar *status)
{
	if (g_strcmp0(status, "ok") == 0)
		return "Read";
	if (g_strcmp0(status, "timeout") == 0)
		return "Reading timed out";
	if (g_strcmp0(status, "failed") == 0)
		return "Could not be read";
	return "Not read yet";
}

static HtmxNode *
status_badge(const gchar *status)
{
	const gchar *css = "badge-note";

	if (g_strcmp0(status, "ok") == 0)
		css = "scan-complete";
	else if (g_strcmp0(status, "failed") == 0 || g_strcmp0(status, "timeout") == 0)
		css = "badge-failed";
	return badge(status_label(status), css);
}

/* The HDR class of a stream or a file, with a warning badge when it is
 * not certain. */
static HtmxNode *
hdr_node(
	AxisHdrFormat format,
	gboolean      uncertain
){
	HtmxSpan *span = htmx_span_new();

	ADD(span, badge(axis_hdr_format_label(format), format == AXIS_HDR_SDR && !uncertain ? "scan-complete" : NULL));
	if (uncertain)
		ADD(span, badge("Uncertain", "badge-failed"));
	return HTMX_NODE(span);
}

/* ── The file page ──────────────────────────────────────────────── */

static HtmxNode *
streams_card(const AxisMediaInfo *info)
{
	static const AxisWebColumn video_columns[] = {
		{ "#", AXIS_WEB_COL_COUNT },
		{ "Codec", AXIS_WEB_COL_WIDE },
		{ "Picture size", AXIS_WEB_COL_TEXT },
		{ "Frame rate", AXIS_WEB_COL_TEXT },
		{ "Bit depth", AXIS_WEB_COL_NUMBER },
		{ "HDR", AXIS_WEB_COL_STATE },
		{ "Bitrate", AXIS_WEB_COL_TEXT },
		{ NULL, 0 }
	};
	static const AxisWebColumn audio_columns[] = {
		{ "#", AXIS_WEB_COL_COUNT },
		{ "Codec", AXIS_WEB_COL_WIDE },
		{ "Channels", AXIS_WEB_COL_TEXT },
		{ "Sample rate", AXIS_WEB_COL_TEXT },
		{ "Bit depth", AXIS_WEB_COL_NUMBER },
		{ "Lossless or lossy", AXIS_WEB_COL_TEXT },
		{ "Bitrate", AXIS_WEB_COL_TEXT },
		{ "Language", AXIS_WEB_COL_TEXT },
		{ "Title", AXIS_WEB_COL_NAME },
		{ NULL, 0 }
	};
	static const AxisWebColumn subtitle_columns[] = {
		{ "#", AXIS_WEB_COL_COUNT },
		{ "Codec", AXIS_WEB_COL_WIDE },
		{ "Type", AXIS_WEB_COL_TEXT },
		{ "Language", AXIS_WEB_COL_TEXT },
		{ "Title", AXIS_WEB_COL_NAME },
		{ "Flags", AXIS_WEB_COL_TEXT },
		{ NULL, 0 }
	};
	static const AxisWebColumn other_columns[] = {
		{ "#", AXIS_WEB_COL_COUNT },
		{ "Type", AXIS_WEB_COL_TEXT },
		{ "Codec", AXIS_WEB_COL_WIDE },
		{ NULL, 0 }
	};
	static const gchar *const kind_names[] = { "video-streams", "audio-streams", "subtitle-streams" };
	HtmxNode *card = axis_web_storage_card("Video, audio and subtitle tracks (streams)");
	AxisStreamKind kinds[3];
	guint k;
	guint i;
	gboolean others = FALSE;

	kinds[0] = AXIS_STREAM_VIDEO;
	kinds[1] = AXIS_STREAM_AUDIO;
	kinds[2] = AXIS_STREAM_SUBTITLE;

	for (k = 0; k < G_N_ELEMENTS(kinds); k++) {
		const AxisWebColumn *columns = k == 0 ? video_columns : k == 1 ? audio_columns : subtitle_columns;
		HtmxNode *table = NULL;
		HtmxTbody *body = NULL;

		for (i = 0; i < info->streams->len; i++) {
			const AxisMediaStream *s = g_ptr_array_index(info->streams, i);
			HtmxTr *row;
			g_autofree gchar *index = g_strdup_printf("%d", s->index);
			g_autofree gchar *codec = s->profile != NULL ?
				g_strdup_printf("%s (%s)", s->codec != NULL ? s->codec : "unknown", s->profile) :
				g_strdup(s->codec != NULL ? s->codec : "unknown");

			if (s->kind != kinds[k])
				continue;
			if (table == NULL) {
				table = axis_web_table_new(kind_names[k], columns);
				body = htmx_tbody_new();
			}
			row = htmx_tr_new();
			cell(HTMX_NODE(row), index);
			cell_isolated(HTMX_NODE(row), codec);
			if (s->kind == AXIS_STREAM_VIDEO) {
				g_autofree gchar *size = format_size(s->width, s->height);
				g_autofree gchar *rate = format_rate(s->frame_rate);
				g_autofree gchar *depth = s->bit_depth > 0 ? g_strdup_printf("%d", s->bit_depth) :
					g_strdup("unknown");
				g_autofree gchar *bitrate = format_bitrate(s->bitrate, s->bitrate_estimated);

				cell(HTMX_NODE(row), size);
				cell(HTMX_NODE(row), rate);
				cell(HTMX_NODE(row), depth);
				if (s->attached_pic)
					cell(HTMX_NODE(row), "Cover picture");
				else
					cell_node(HTMX_NODE(row), hdr_node(s->hdr_format, s->hdr_uncertain));
				cell(HTMX_NODE(row), bitrate);
			} else if (s->kind == AXIS_STREAM_AUDIO) {
				g_autofree gchar *channels = s->channels > 0 ?
					g_strdup_printf("%d%s%s", s->channels, s->channel_layout != NULL ? " " : "",
						s->channel_layout != NULL ? s->channel_layout : "") : g_strdup("unknown");
				g_autofree gchar *rate = s->sample_rate > 0 ? g_strdup_printf("%d Hz", s->sample_rate) :
					g_strdup("unknown");
				g_autofree gchar *depth = s->bit_depth_audio > 0 ? g_strdup_printf("%d", s->bit_depth_audio) :
					g_strdup("n/a");
				g_autofree gchar *bitrate = format_bitrate(s->bitrate, s->bitrate_estimated);

				cell_isolated(HTMX_NODE(row), channels);
				cell(HTMX_NODE(row), rate);
				cell(HTMX_NODE(row), depth);
				cell(HTMX_NODE(row), tri_text(s->lossless, "Lossless", "Lossy"));
				cell(HTMX_NODE(row), bitrate);
				cell_isolated(HTMX_NODE(row), s->language);
				cell_isolated(HTMX_NODE(row), s->title);
			} else {
				g_autofree gchar *flags = g_strdup_printf("%s%s%s", s->is_default ? "default" : "",
					s->is_default && s->is_forced ? ", " : "", s->is_forced ? "forced" : "");

				cell(HTMX_NODE(row), s->subtitle_kind == AXIS_SUBTITLE_TEXT ? "Text" :
					s->subtitle_kind == AXIS_SUBTITLE_IMAGE ? "Image" : "unknown");
				cell_isolated(HTMX_NODE(row), s->language);
				cell_isolated(HTMX_NODE(row), s->title);
				cell(HTMX_NODE(row), flags);
			}
			ADD(body, row);
		}
		if (table != NULL) {
			ADD(card, axis_web_storage_text("scan-subhead", k == 0 ? "Video" : k == 1 ? "Audio" : "Subtitles"));
			ADD(table, body);
			ADD(card, table);
		}
	}

	for (i = 0; i < info->streams->len; i++) {
		const AxisMediaStream *s = g_ptr_array_index(info->streams, i);

		if (s->kind != AXIS_STREAM_VIDEO && s->kind != AXIS_STREAM_AUDIO && s->kind != AXIS_STREAM_SUBTITLE)
			others = TRUE;
	}
	if (others) {
		HtmxNode *table = axis_web_table_new("other-streams", other_columns);
		HtmxTbody *body = htmx_tbody_new();

		for (i = 0; i < info->streams->len; i++) {
			const AxisMediaStream *s = g_ptr_array_index(info->streams, i);
			HtmxTr *row;
			g_autofree gchar *index = g_strdup_printf("%d", s->index);

			if (s->kind == AXIS_STREAM_VIDEO || s->kind == AXIS_STREAM_AUDIO || s->kind == AXIS_STREAM_SUBTITLE)
				continue;
			row = htmx_tr_new();
			cell(HTMX_NODE(row), index);
			cell(HTMX_NODE(row), axis_stream_kind_to_string(s->kind));
			cell_isolated(HTMX_NODE(row), s->codec);
			ADD(body, row);
		}
		ADD(card, axis_web_storage_text("scan-subhead", "Other streams"));
		ADD(table, body);
		ADD(card, table);
	}
	if (info->streams_omitted > 0) {
		g_autofree gchar *more = g_strdup_printf("%d more streams are not listed.", info->streams_omitted);

		ADD(card, axis_web_storage_text("help", more));
	}
	return card;
}

static HtmxNode *
picture_card(const AxisMediaInfo *info)
{
	HtmxNode *card = axis_web_storage_card("Main video");
	HtmxNode *facts = facts_new();
	g_autofree gchar *size = format_size(info->width, info->height);
	g_autofree gchar *depth = info->bit_depth > 0 ? g_strdup_printf("%d", info->bit_depth) : g_strdup("unknown");
	g_autofree gchar *rate = format_rate(info->frame_rate);
	g_autofree gchar *bpp = info->bits_per_pixel >= 0 ?
		g_strdup_printf("%.4f%s", info->bits_per_pixel, info->bits_per_pixel_estimated ? " (estimated)" : "") :
		g_strdup("unknown");

	if (info->hdr_uncertain) {
		ADD(card, warning(info->hdr_note != NULL ? info->hdr_note :
			"The HDR format of this video could not be told for certain."));
		ADD(card, axis_web_storage_text("help", "Until it is certain, Axis treats this video as one it must "
			"not convert."));
	}
	fact_node(facts, "HDR format", hdr_node(info->hdr_format, info->hdr_uncertain));
	fact(facts, "Codec", info->video_codec);
	fact(facts, "Picture size", size);
	fact(facts, "Resolution", axis_probe_resolution_label(info->resolution_class));
	fact(facts, "Bit depth", depth);
	fact(facts, "Frame rate", rate);
	fact(facts, "Bits per pixel per frame", bpp);
	fact(facts, "Interlaced", tri_text(info->interlaced, "Yes", "No"));
	fact(facts, "Variable frame rate", tri_text(info->vfr_suspected, "Suspected", "No"));
	ADD(card, facts);
	return card;
}

static HtmxNode *
tags_card(const AxisMediaInfo *info)
{
	HtmxNode *card = axis_web_storage_card("Tags and cover art");
	HtmxNode *facts = facts_new();

	if (info->tag_title != NULL)
		fact_node(facts, "Title", isolated(info->tag_title));
	if (info->tag_artist != NULL)
		fact_node(facts, "Artist", isolated(info->tag_artist));
	if (info->tag_album != NULL)
		fact_node(facts, "Album", isolated(info->tag_album));
	fact(facts, "Track number", info->has_track ? "Present" : "None");
	fact(facts, "Disc number", info->has_disc ? "Present" : "None");
	fact(facts, "MusicBrainz identifiers", info->has_musicbrainz ? "Present" : "None");
	fact(facts, "ReplayGain", info->has_replaygain ? "Present" : "None");
	if (info->has_embedded_art) {
		g_autofree gchar *art = info->art_width > 0 ? g_strdup_printf("Present, %d x %d", info->art_width,
			info->art_height) : g_strdup("Present");

		fact(facts, "Cover art", art);
	} else {
		fact(facts, "Cover art", "None");
	}
	ADD(card, facts);
	return card;
}

static HtmxNode *
probe_card(
	HtmxRequest        *request,
	const AxisFileView *view,
	gboolean            volume_usable,
	AxisWebNotice      *placed
){
	HtmxNode *card = axis_web_storage_card("Reading this file (probe)");
	HtmxNode *facts = facts_new();
	const AxisMediaInfo *info = view->info;
	g_autofree gchar *action = g_strdup_printf("/library/file/%" G_GINT64_FORMAT "/probe", view->file_id);
	HtmxNode *form;

	htmx_element_set_id(HTMX_ELEMENT(card), "file-probe");
	axis_web_notice_put(placed, card, "file-probe");
	if (info == NULL) {
		fact_node(facts, "Status", badge(view->waiting ? "Waiting" : "Not read yet", "badge-note"));
	} else {
		g_autofree gchar *when = axis_web_storage_format_time(info->probed_at_us, "unknown");
		g_autofree gchar *took = info->probe_ms >= 0 ? g_strdup_printf("%d ms", info->probe_ms) : g_strdup("");

		fact_node(facts, "Status", status_badge(info->status));
		fact(facts, "Read at", when);
		if (info->probe_ms >= 0)
			fact(facts, "Took", took);
		if (info->tool_version != NULL)
			fact_node(facts, "Tool", isolated(info->tool_version));
		{
			g_autofree gchar *version = g_strdup_printf("%d", info->probe_version);

			fact(facts, "Probe version", version);
		}
	}
	if (view->waiting && info != NULL)
		fact(facts, "Waiting", "This file is waiting to be read again.");
	ADD(card, facts);

	if (info != NULL && info->error != NULL) {
		HtmxP *line = htmx_p_new();

		htmx_element_add_class(HTMX_ELEMENT(line), "job-error");
		ADD(line, isolated(info->error));
		ADD(card, line);
		ADD(card, axis_web_storage_text("help", "After a failed read, Axis does not read the file again "
			"until it changes or an administrator asks."));
	}
	if (view->delayed_reason != NULL) {
		g_autofree gchar *until = axis_web_storage_format_time(view->delayed_until_us, "later");
		g_autofree gchar *text = g_strdup_printf("The next read was put off until %s: %s", until,
			view->delayed_reason);

		ADD(card, axis_web_storage_text("help", text));
	}

	if (axis_web_request_is_admin(request) && volume_usable) {
		form = axis_web_auth_post_form(request, action);
		htmx_element_add_class(HTMX_ELEMENT(form), "inline-form");
		axis_web_auth_add(form, axis_web_auth_button("Read this file again", NULL));
		ADD(card, form);
		ADD(card, axis_web_storage_text("help", "Looks inside the file again. It only reads: nothing on the "
			"disk is changed."));
	} else if (axis_web_request_is_admin(request)) {
		HtmxButton *button = htmx_button_new_submit();

		htmx_node_set_text_content(HTMX_NODE(button), "Read this file again");
		htmx_button_set_disabled(button, TRUE);
		ADD(card, button);
		ADD(card, axis_web_storage_text("help", "The disk is offline, so Axis cannot read the file now. What "
			"is shown is what was last known."));
	}
	return card;
}

static HtmxNode *
container_card(const AxisMediaInfo *info)
{
	HtmxNode *card = axis_web_storage_card("Format and length (container)");
	HtmxNode *facts = facts_new();
	g_autofree gchar *duration = format_duration(info->duration_ms);
	g_autofree gchar *bitrate = format_bitrate(info->bitrate, info->bitrate_estimated);
	g_autofree gchar *chapters = info->chapter_count >= 0 ? g_strdup_printf("%d", info->chapter_count) :
		g_strdup("unknown");
	g_autofree gchar *counts = g_strdup_printf("%d video, %d audio, %d subtitle", info->video_streams,
		info->audio_streams, info->subtitle_streams);

	fact_node(facts, "Format", isolated(info->container));
	if (info->video_streams > 0 && info->tag_title != NULL)
		fact_node(facts, "Title", isolated(info->tag_title));
	fact(facts, "Duration", duration);
	fact(facts, "Overall bitrate", bitrate);
	fact(facts, "Chapters", chapters);
	fact(facts, "Streams", counts);
	if (info->audio_bitrate_total > 0) {
		g_autofree gchar *audio = format_bitrate(info->audio_bitrate_total, info->audio_bitrate_total_estimated);

		fact(facts, "All audio together", audio);
	}
	ADD(card, facts);
	return card;
}

/* Where the index stands about the file, in words and with dates. */
static gchar *
scan_state_text(const AxisFileView *view)
{
	g_autofree gchar *suspected = axis_web_storage_format_time(view->suspected_us, "an unknown time");
	g_autofree gchar *missing = axis_web_storage_format_time(view->missing_us, "an unknown time");

	if (g_strcmp0(view->state, "present") == 0)
		return g_strdup("Present: the last complete scan found it");
	if (g_strcmp0(view->state, "suspected") == 0)
		return g_strdup_printf("Not found by the last scan (suspected): the complete scan of %s did not "
			"find it. If the next complete scan an hour or more later does not find it either, it is "
			"marked missing", suspected);
	return g_strdup_printf("Missing: not found by complete scans since %s (marked missing %s). Its "
		"record is removed 30 days after that", suspected, missing);
}

static HtmxNode *
file_card(
	AxisWebProbe       *self,
	const AxisFileView *view,
	const AxisRootInfo *root
){
	HtmxNode *card = axis_web_storage_card("File on disk");
	HtmxNode *facts = facts_new();
	g_autofree gchar *size = axis_size_format((guint64)view->size);
	g_autofree gchar *when = axis_web_storage_format_time(view->mtime_us, "unknown");
	g_autofree gchar *first = axis_web_storage_format_time(view->first_seen_us, "unknown");
	g_autofree gchar *links = g_strdup_printf("%" G_GINT64_FORMAT, view->nlink);
	g_autofree gchar *root_href = g_strdup_printf("/library/root/%" G_GINT64_FORMAT, view->root_id);
	g_autofree gchar *state = scan_state_text(view);
	g_autofree gchar *shown = axis_library_name_display(view->rel_path);
	g_autofree gchar *exact = axis_library_name_exact(view->rel_path);
	g_autofree gchar *kind = NULL;
	const gchar *slash = strrchr(view->rel_path, '/');
	AxisFileKind parsed;

	kind = view->kind != NULL && axis_file_kind_from_string(view->kind, &parsed) ?
		g_strdup(axis_file_kind_label_one(parsed)) : g_strdup(view->kind);

	if (root != NULL)
		fact_node(facts, "Library folder", link_to(root_href, root->name));
	if (slash != NULL) {
		g_autofree gchar *directory = g_strndup(view->rel_path, (gsize)(slash - view->rel_path));
		g_autofree gchar *folder_href = g_strdup_printf("/library/folder/%" G_GINT64_FORMAT, view->dir_id);

		if (view->dir_id > 0) {
			HtmxA *link = htmx_a_new_with_href(folder_href);

			ADD(link, path_node(directory));
			fact_node(facts, "Folder", HTMX_NODE(link));
		} else {
			fact_node(facts, "Folder", path_node(directory));
		}
	} else if (view->dir_id > 0) {
		g_autofree gchar *folder_href = g_strdup_printf("/library/folder/%" G_GINT64_FORMAT, view->dir_id);

		fact_node(facts, "Folder", link_to(folder_href, "The top of the library folder"));
	}
	fact_node(facts, "Path", path_node(view->rel_path));
	/* The exact bytes where the page's text is not them: a name that is
	 * not valid UTF-8, or that holds control or direction characters. */
	if (view->name_invalid || strcmp(shown, exact) != 0) {
		HtmxCode *code = htmx_code_new();

		htmx_element_add_class(HTMX_ELEMENT(code), "scan-path");
		ADD(code, isolated(exact));
		fact_node(facts, "Exact name", HTMX_NODE(code));
		if (view->name_invalid)
			fact(facts, "Name encoding", "Not valid UTF-8. Axis keeps the name exactly as its bytes and never renames "
				"it; each byte that is not UTF-8 is written \\xHH above.");
	}
	fact(facts, "Type", kind);
	if (view->link_target != NULL)
		fact_node(facts, "Points to", path_node(view->link_target));
	fact(facts, "Size", size);
	fact(facts, "Modified", when);
	fact(facts, "Hard links", links);
	fact(facts, "First seen by a scan", first);
	fact(facts, "Scan state", state);
	if (view->pinned_links && view->pinned_downloads)
		fact(facts, "Pinned", "Yes: it has more than one hard link, and it is in a library "
			"folder that holds downloads");
	else if (view->pinned_links)
		fact(facts, "Pinned", "Yes: it has more than one hard link");
	else if (view->pinned_downloads)
		fact(facts, "Pinned", "Yes: it is in a library folder that holds downloads");
	else
		fact(facts, "Pinned", "No");
	ADD(card, facts);
	if (view->pinned)
		ADD(card, axis_web_storage_text("help", "Axis does not convert, move or delete a pinned file: "
			"changing it would change what the other link, or the download, sees."));
	return card;
}

typedef struct
{
	gint64         id;
	AxisFileView  *view;
	AxisRootInfo  *root;
	gint64         job_id;
	gboolean       offline;
	GArray        *estimates;	/* AxisFileEstimate, or NULL for no card */
	gboolean       estimate_failed;
	gpointer       card;	/* another module's card's figures */
	GDestroyNotify card_free;
	GError        *error;
} FileWork;

static void
file_work_free(gpointer data)
{
	FileWork *work = data;

	if (work->card != NULL && work->card_free != NULL)
		work->card_free(work->card);
	axis_file_view_free(work->view);
	axis_root_info_free(work->root);
	g_clear_pointer(&work->estimates, g_array_unref);
	g_clear_error(&work->error);
	g_free(work);
}

static const gchar *
done_message(const gchar *key)
{
	if (g_strcmp0(key, "probe-requested") == 0)
		return "Axis will read this file again when its disk's turn comes; reload this "
			"page to see the result.";
	return NULL;
}

static HtmxResponse *
file_page(
	AxisWebProbe  *self,
	HtmxRequest   *request,
	FileWork      *work
){
	const AxisFileView *view = work->view;
	g_autoptr(HtmxDiv) content = htmx_div_new();
	g_autofree gchar *name = NULL;
	g_autofree gchar *shown = NULL;
	g_autofree gchar *root_href = g_strdup_printf("/library/root/%" G_GINT64_FORMAT, view->root_id);
	const gchar *notice = done_message(htmx_request_get_query_param(request, "done"));
	const gchar *slash = strrchr(view->rel_path, '/');
	gboolean media = g_strcmp0(view->kind, "video") == 0 || g_strcmp0(view->kind, "audio") == 0;
	HtmxHeading *title = htmx_h1_new();
	HtmxP *crumbs = htmx_p_new();
	AxisWebNotice placed;

	name = axis_library_name_display(slash != NULL ? slash + 1 : view->rel_path);
	shown = g_strdup(name);

	ADD(crumbs, link_to("/library", "Files"));
	if (work->root != NULL) {
		HtmxSpan *separator = htmx_span_new();

		htmx_node_set_text_content(HTMX_NODE(separator), " / ");
		ADD(crumbs, separator);
		ADD(crumbs, link_to(root_href, work->root->name));
	}
	ADD(content, crumbs);
	ADD(title, isolated(shown));
	ADD(content, title);
	axis_web_notice_begin(&placed, request, FALSE, g_strdup(notice), HTMX_NODE(content));
	if (g_strcmp0(view->state, "present") != 0)
		ADD(content, warning("The last complete scan did not find this file. What is shown was true when it "
			"was last seen."));

	ADD(content, file_card(self, view, work->root));
	/* A video file of a library folder for movies or TV can be linked to a
	 * movie or an episode by hand (the Movies module's page). */
	if (axis_web_request_is_admin(request) && g_strcmp0(view->kind, "video") == 0 &&
	    g_strcmp0(view->state, "present") == 0 && work->root != NULL &&
	    (work->root->media_type == AXIS_MEDIA_MOVIES || work->root->media_type == AXIS_MEDIA_TV)) {
		g_autofree gchar *by_hand = g_strdup_printf("/library/file/%" G_GINT64_FORMAT "/link", work->id);
		HtmxP *line = htmx_p_new();

		ADD(line, link_to(by_hand, "Link to a movie or episode by hand"));
		ADD(content, line);
	}
	if (!media)
		ADD(content, axis_web_storage_text("help", "Axis looks inside video and audio files only; there "
			"is nothing more to know about this one."));
	else {
		g_autoptr(AxisVolumeInfo) volume = axis_storage_get_volume(self->storage, view->volume_id);

		ADD(content, probe_card(request, view, volume != NULL && axis_volume_state_is_usable(volume->state),
			&placed));
	}
	if (view->info != NULL && g_strcmp0(view->info->status, "ok") == 0) {
		const AxisMediaInfo *info = view->info;

		ADD(content, container_card(info));
		if (info->video_streams > 0)
			ADD(content, picture_card(info));
		ADD(content, streams_card(info));
		/* A film's cover is its poster: tags are for music and books. */
		if (info->video_streams == 0 &&
		    (info->tag_title != NULL || info->tag_artist != NULL || info->tag_album != NULL ||
		     info->has_embedded_art || info->has_track || info->has_replaygain || info->has_musicbrainz))
			ADD(content, tags_card(info));
	}
	/* What each conversion target would make of it (the Savings module). */
	if (work->estimates != NULL) {
		ADD(content, axis_web_savings_file_card(work->estimates));
	} else if (work->estimate_failed) {
		HtmxNode *card = axis_web_storage_card("Space saved if converted");

		ADD(card, axis_web_storage_text("help", "The estimates could not be worked out just now. Reload "
			"the page to try again."));
		ADD(content, card);
	}
	/* Another module's card (the conversions'). */
	if (self->card_render != NULL && work->card != NULL) {
		HtmxNode *card = self->card_render(request, work->id, work->card, self->card_data);

		if (card != NULL)
			ADD(content, card);
	}

	axis_web_notice_end(&placed);
	return axis_web_page_respond(request, shown, HTMX_NODE(content));
}

static void
file_view_run(
	AxisWebProbe *self,
	gpointer      data
){
	FileWork *work = data;

	if (!axis_probe_get_file_view(self->probe, work->id, &work->view, &work->error))
		return;
	axis_storage_get_root(self->storage, work->view->root_id, &work->root, NULL);
	if (self->estimator != NULL && axis_estimator_is_enabled(self->estimator) &&
	    (g_strcmp0(work->view->kind, "video") == 0 || g_strcmp0(work->view->kind, "audio") == 0)) {
		g_autoptr(GError) error = NULL;

		if (!axis_estimator_estimate_file(self->estimator, work->id, &work->estimates, &error)) {
			g_debug("file page: no estimate for file %" G_GINT64_FORMAT ": %s", work->id, error->message);
			work->estimate_failed = TRUE;
		}
	}
	if (self->card_collect != NULL) {
		work->card = self->card_collect(work->id, work->view->kind, self->card_data);
		work->card_free = self->card_free;
	}
}

static HtmxResponse *
file_view_done(
	AxisWebProbe *self,
	HtmxRequest  *request,
	gpointer      data
){
	FileWork *work = data;

	if (g_error_matches(work->error, AXIS_PROBE_ERROR, AXIS_PROBE_ERROR_NOT_FOUND))
		return not_found(request);
	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	return file_page(self, request, work);
}

static HtmxResponse *
handle_file(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	gint64 id = axis_web_storage_param_id(params);
	FileWork *work;

	if (id == 0)
		return not_found(request);

	work = g_new0(FileWork, 1);
	work->id = id;
	return dispatch(user_data, request, file_view_run, file_view_done, work, file_work_free);
}

/* ── Probe again ────────────────────────────────────────────────── */

static void
file_request_run(
	AxisWebProbe *self,
	gpointer      data
){
	FileWork *work = data;
	g_autoptr(AxisVolumeInfo) volume = NULL;

	/* Nothing is asked of a volume that is not there. */
	if (!axis_probe_get_file_view(self->probe, work->id, &work->view, &work->error))
		return;
	volume = axis_storage_get_volume(self->storage, work->view->volume_id);
	if (volume == NULL || !axis_volume_state_is_usable(volume->state)) {
		work->offline = TRUE;
		return;
	}
	axis_probe_request_file(self->probe, work->id, &work->job_id, &work->error);
}

static HtmxResponse *
file_request_done(
	AxisWebProbe *self,
	HtmxRequest  *request,
	gpointer      data
){
	FileWork *work = data;
	g_autofree gchar *location = NULL;

	if (g_error_matches(work->error, AXIS_PROBE_ERROR, AXIS_PROBE_ERROR_NOT_FOUND))
		return not_found(request);
	if (work->offline)
		return axis_web_error_respond(request, 409, "The disk is offline",
			"Axis cannot scan or read files on a disk that is offline. Its files are shown as last known; "
			"try again when the disk is back online.");
	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);

	axis_web_storage_audit(request, "probe requested: file=%" G_GINT64_FORMAT " job=%" G_GINT64_FORMAT,
		work->id, work->job_id);
	location = g_strdup_printf("/library/file/%" G_GINT64_FORMAT, work->id);
	return axis_web_place_redirect(request, location, "probe-requested", "file-probe");
}

static HtmxResponse *
handle_file_probe(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	gint64 id = axis_web_storage_param_id(params);
	FileWork *work;

	if (id == 0)
		return not_found(request);

	work = g_new0(FileWork, 1);
	work->id = id;
	return dispatch(user_data, request, file_request_run, file_request_done, work, file_work_free);
}

/* ── A root's probe page ────────────────────────────────────────── */

typedef struct
{
	gint64              id;
	AxisRootInfo       *root;
	AxisRootProbeView  *view;
	guint               requested;
	gboolean            capped;
	GError             *error;
} RootWork;

static void
root_work_free(gpointer data)
{
	RootWork *work = data;

	axis_root_info_free(work->root);
	axis_root_probe_view_free(work->view);
	g_clear_error(&work->error);
	g_free(work);
}

static HtmxNode *
listing_table(
	GPtrArray   *list,
	const gchar *last_heading
){
	AxisWebColumn columns[5];
	HtmxNode *table;
	HtmxTbody *body = htmx_tbody_new();
	guint i;

	/* One kind for every listing of the page, whatever its third heading. */
	columns[0].heading = "File";
	columns[0].role = AXIS_WEB_COL_NAME;
	columns[1].heading = "Status";
	columns[1].role = AXIS_WEB_COL_STATE;
	columns[2].heading = last_heading;
	columns[2].role = AXIS_WEB_COL_WIDE;
	columns[3].heading = "When";
	columns[3].role = AXIS_WEB_COL_DATE;
	columns[4].heading = NULL;
	columns[4].role = 0;
	table = axis_web_table_new("probe-listing", columns);
	for (i = 0; i < list->len; i++) {
		const AxisProbeListing *listing = g_ptr_array_index(list, i);
		g_autofree gchar *href = g_strdup_printf("/library/file/%" G_GINT64_FORMAT, listing->file_id);
		g_autofree gchar *shown = axis_library_name_display(listing->rel_path);
		g_autofree gchar *when = axis_web_storage_format_time(listing->probed_at_us, "unknown");
		HtmxTr *row = htmx_tr_new();
		HtmxTd *name = htmx_td_new();
		HtmxA *link = htmx_a_new_with_href(href);

		htmx_element_add_class(HTMX_ELEMENT(name), "scan-path");
		ADD(link, isolated(shown));
		ADD(name, link);
		ADD(row, name);
		cell_node(HTMX_NODE(row), status_badge(listing->status));
		cell_isolated(HTMX_NODE(row), listing->text);
		cell(HTMX_NODE(row), when);
		ADD(body, row);
	}
	ADD(table, body);
	return table;
}

static HtmxNode *
counts_card(const AxisRootProbeView *view)
{
	HtmxNode *card = axis_web_storage_card("How many files Axis has read");
	HtmxNode *facts = facts_new();
	g_autofree gchar *media = format_count(view->media_files);
	g_autofree gchar *probed = format_count(view->probed);
	g_autofree gchar *failed = format_count(view->failed);
	g_autofree gchar *timed_out = format_count(view->timed_out);
	g_autofree gchar *never = format_count(view->never_probed);
	g_autofree gchar *waiting = format_count(view->waiting);
	g_autofree gchar *delayed = format_count(view->delayed);
	g_autofree gchar *stale = format_count(view->stale);
	g_autofree gchar *uncertain = format_count(view->hdr_uncertain);

	ADD(card, axis_web_storage_text("help", "Video and audio files that the last complete scan found, and "
		"what looking inside them found."));
	fact(facts, "Video and audio files", media);
	fact(facts, "Read", probed);
	fact(facts, "Could not be read", failed);
	fact(facts, "Reading timed out", timed_out);
	fact(facts, "Not read yet", never);
	fact(facts, "Waiting to be read", waiting);
	if (view->delayed > 0)
		fact(facts, "Of those, put off for now", delayed);
	if (view->stale > 0)
		fact(facts, "Read by an older version of Axis", stale);
	fact(facts, "HDR format uncertain", uncertain);
	ADD(card, facts);
	return card;
}

static HtmxResponse *
root_page(
	HtmxRequest *request,
	RootWork    *work
){
	const AxisRootProbeView *view = work->view;
	g_autoptr(HtmxDiv) content = htmx_div_new();
	g_autofree gchar *root_href = g_strdup_printf("/storage/roots/%" G_GINT64_FORMAT, work->root->id);
	g_autofree gchar *action = g_strdup_printf("/storage/roots/%" G_GINT64_FORMAT "/probe", work->root->id);
	g_autofree gchar *heading = g_strdup_printf("Files read in %s", work->root->name);
	HtmxHeading *title = htmx_h1_new();
	HtmxP *crumbs = htmx_p_new();
	HtmxSpan *separator = htmx_span_new();
	const gchar *done = htmx_request_get_query_param(request, "done");
	AxisWebNotice placed;

	ADD(crumbs, link_to("/storage/roots", "Back to Library folders"));
	htmx_node_set_text_content(HTMX_NODE(separator), " / ");
	ADD(crumbs, separator);
	ADD(crumbs, link_to(root_href, work->root->name));
	ADD(content, crumbs);
	htmx_node_set_text_content(HTMX_NODE(title), heading);
	ADD(content, title);
	axis_web_notice_begin(&placed, request, FALSE, g_strcmp0(done, "probe-requested") == 0 ?
		g_strdup("Axis will read the failed files again, one disk at a time. Reload this page to see "
			"the results.") : NULL, HTMX_NODE(content));

	ADD(content, counts_card(view));

	if (view->failures->len > 0) {
		HtmxNode *card = axis_web_storage_card("Files Axis could not read");

		htmx_element_set_id(HTMX_ELEMENT(card), "probe-failed");
		axis_web_notice_put(&placed, card, "probe-failed");
		ADD(card, axis_web_storage_text("help", "Axis does not read these again until they change or an "
			"administrator asks."));
		ADD(card, listing_table(view->failures, "Why"));
		if (view->failed + view->timed_out > view->failures->len) {
			g_autofree gchar *more = g_strdup_printf("%" G_GUINT64_FORMAT " more files are not listed.",
				(guint64)(view->failed + view->timed_out - view->failures->len));

			ADD(card, axis_web_storage_text("help", more));
		}
		if (axis_web_request_is_admin(request)) {
			HtmxNode *form = axis_web_auth_post_form(request, action);

			htmx_element_add_class(HTMX_ELEMENT(form), "inline-form");
			axis_web_auth_add(form, axis_web_auth_button("Read the failed files again", NULL));
			ADD(card, form);
		}
		ADD(content, card);
	}
	if (view->recent->len > 0) {
		HtmxNode *card = axis_web_storage_card("Files read most recently");

		ADD(card, listing_table(view->recent, "Format"));
		ADD(content, card);
	}

	axis_web_notice_end(&placed);
	return axis_web_page_respond(request, heading, HTMX_NODE(content));
}

static void
root_view_run(
	AxisWebProbe *self,
	gpointer      data
){
	RootWork *work = data;

	if (!axis_storage_get_root(self->storage, work->id, &work->root, &work->error))
		return;
	axis_probe_get_root_view(self->probe, work->id, &work->view, &work->error);
}

static HtmxResponse *
root_view_done(
	AxisWebProbe *self,
	HtmxRequest  *request,
	gpointer      data
){
	RootWork *work = data;

	if (g_error_matches(work->error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_FOUND))
		return not_found(request);
	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	return root_page(request, work);
}

static HtmxResponse *
handle_root(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	gint64 id = axis_web_storage_param_id(params);
	RootWork *work;

	if (id == 0)
		return not_found(request);

	work = g_new0(RootWork, 1);
	work->id = id;
	return dispatch(user_data, request, root_view_run, root_view_done, work, root_work_free);
}

static void
root_request_run(
	AxisWebProbe *self,
	gpointer      data
){
	RootWork *work = data;

	if (!axis_storage_get_root(self->storage, work->id, &work->root, &work->error))
		return;
	axis_probe_request_failed(self->probe, work->id, &work->requested, &work->capped, &work->error);
}

static HtmxResponse *
root_request_done(
	AxisWebProbe *self,
	HtmxRequest  *request,
	gpointer      data
){
	RootWork *work = data;
	g_autofree gchar *location = NULL;

	if (g_error_matches(work->error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_FOUND))
		return not_found(request);
	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);

	axis_web_storage_audit(request, "probe of failed files requested: root=%" G_GINT64_FORMAT " files=%u",
		work->id, work->requested);
	location = g_strdup_printf("/storage/roots/%" G_GINT64_FORMAT "/probe", work->id);
	return axis_web_place_redirect(request, location, "probe-requested", "probe-failed");
}

static HtmxResponse *
handle_root_probe(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	gint64 id = axis_web_storage_param_id(params);
	RootWork *work;

	if (id == 0)
		return not_found(request);

	work = g_new0(RootWork, 1);
	work->id = id;
	return dispatch(user_data, request, root_request_run, root_request_done, work, root_work_free);
}

/* ── Lifetime ───────────────────────────────────────────────────── */

AxisWebProbe *
axis_web_probe_new(
	AxisProbe   *probe,
	AxisStorage *storage
){
	AxisWebProbe *self;

	g_return_val_if_fail(probe != NULL, NULL);
	g_return_val_if_fail(storage != NULL, NULL);

	self = g_new0(AxisWebProbe, 1);
	self->probe = axis_probe_ref(probe);
	self->storage = axis_storage_ref(storage);
	return self;
}

void
axis_web_probe_stop(AxisWebProbe *self)
{
	g_return_if_fail(self != NULL);

	self->stopping = TRUE;
	while (self->pending > 0)
		g_main_context_iteration(NULL, TRUE);
}

void
axis_web_probe_free(AxisWebProbe *self)
{
	if (self == NULL)
		return;

	axis_web_probe_stop(self);
	axis_probe_unref(self->probe);
	axis_storage_unref(self->storage);
	if (self->estimator != NULL)
		axis_estimator_unref(self->estimator);
	g_free(self);
}

void
axis_web_probe_set_estimator(
	AxisWebProbe  *self,
	AxisEstimator *estimator
){
	g_return_if_fail(self != NULL);

	if (estimator != NULL)
		axis_estimator_ref(estimator);
	if (self->estimator != NULL)
		axis_estimator_unref(self->estimator);
	self->estimator = estimator;
}

void
axis_web_probe_set_file_card(
	AxisWebProbe           *self,
	AxisWebFileCardCollect  collect,
	AxisWebFileCardRender   render,
	GDestroyNotify          free_figures,
	gpointer                user_data
){
	g_return_if_fail(self != NULL);

	self->card_collect = collect;
	self->card_render = render;
	self->card_free = free_figures;
	self->card_data = user_data;
}

/* ── The module ─────────────────────────────────────────────────── */

static gboolean
file_handler(
	AxisJobContext  *context,
	gpointer         user_data,
	GError         **error
){
	AxisWebProbe *self = user_data;

	return axis_probe_run_file_job(self->probe, context, error);
}

static gboolean
due_handler(
	AxisJobContext  *context,
	gpointer         user_data,
	GError         **error
){
	AxisWebProbe *self = user_data;

	return axis_probe_run_due_job(self->probe, context, error);
}

static gboolean
stale_handler(
	AxisJobContext  *context,
	gpointer         user_data,
	GError         **error
){
	AxisWebProbe *self = user_data;

	return axis_probe_run_stale_job(self->probe, context, error);
}

static void
register_routes(
	AxisWeb  *web,
	gpointer  user_data
){
	axis_web_get(web, "/library/file/:id", handle_file, user_data);
	axis_web_route_full(web, HTMX_METHOD_POST, "/library/file/:id/probe", AXIS_WEB_ROUTE_ADMIN,
		handle_file_probe, user_data);
	axis_web_get(web, "/storage/roots/:id/probe", handle_root, user_data);
	axis_web_route_full(web, HTMX_METHOD_POST, "/storage/roots/:id/probe", AXIS_WEB_ROUTE_ADMIN,
		handle_root_probe, user_data);
}

static const AxisJobHandlerInfo handlers[] = {
	{ AXIS_PROBE_FILE_KIND, AXIS_PROBE_QUEUE, AXIS_PROBE_FILE_ATTEMPTS, file_handler,
	  "Reading what is inside a file", AXIS_JOB_CLASS_WORK },
	/* One attempt: the next turn comes a minute later anyway. */
	{ AXIS_PROBE_DUE_KIND, AXIS_JOB_MAINTENANCE_QUEUE, 1, due_handler,
	  "Looking for new and changed files to read", AXIS_JOB_CLASS_UPKEEP },
	{ AXIS_PROBE_STALE_KIND, AXIS_JOB_MAINTENANCE_QUEUE, 1, stale_handler,
	  "Looking for files read by an older version of Axis", AXIS_JOB_CLASS_UPKEEP },
	{ NULL, NULL, 0, NULL, NULL, AXIS_JOB_CLASS_UNSET }
};

static const AxisJobScheduleInfo schedules[] = {
	{ AXIS_PROBE_DUE_SCHEDULE, AXIS_PROBE_DUE_KIND, NULL, AXIS_PROBE_DUE_INTERVAL_SECONDS },
	{ AXIS_PROBE_STALE_SCHEDULE, AXIS_PROBE_STALE_KIND, NULL, AXIS_PROBE_STALE_INTERVAL_SECONDS },
	{ NULL, NULL, NULL, 0 }
};

static const gchar *const depends_on[] = { AXIS_LIBRARY_MODULE_ID, NULL };

const AxisModuleInfo *
axis_web_probe_get_module(void)
{
	static AxisModuleInfo info;

	if (info.id == NULL) {
		info.id = AXIS_PROBE_MODULE_ID;
		info.name = "File details (probe)";
		info.job_handlers = handlers;
		info.job_schedules = schedules;
		info.register_routes = register_routes;
		info.settings = axis_probe_get_setting_table();
		info.description = "Looks inside every video and audio file Axis has on record, with ffprobe: container, "
			"streams, codecs, HDR and Dolby Vision, audio layout, subtitles and tags. It only reads the "
			"disks, and never guesses: what it cannot tell for certain it marks as uncertain.";
		info.depends_on = depends_on;
	}

	return &info;
}
