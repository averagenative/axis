/*
 * axis-web-savings-pages.c - the Savings pages
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Every page reads the latest snapshot (axis-estimator.h) and says when it
 * was taken and how calibrated its figures are.  Every count of files is a
 * link to the file list of those files (target and outcome, and the
 * figure's other conditions), made only when the list's own parser accepts
 * the query.  Names from the library are text content inside <bdi>; links
 * carry ids and fixed names.  Ranges are written as "about X (A to B)".
 */

#include "web/axis-web-savings-private.h"

#include <string.h>

#include "db/axis-db-error.h"
#include "library/axis-library-name.h"
#include "probe/axis-probe-model.h"
#include "web/axis-web-compat.h"
#include "web/axis-web-notice.h"
#include "web/axis-web-place.h"

/* ── Pieces ─────────────────────────────────────────────────────── */

static HtmxResponse *
not_found(HtmxRequest *request)
{
	return axis_web_error_respond(request, 404, "Not found", "There is nothing at this address.");
}

static HtmxResponse *
bad_request(HtmxRequest *request)
{
	return axis_web_error_respond(request, 400, "Bad request", "The address is not valid.");
}

static HtmxResponse *
fragment_response(HtmxNode *node)
{
	HtmxResponse *response = htmx_response_new();
	g_autofree gchar *markup = NULL;

	axis_web_table_apply_roles(node);
	markup = htmx_element_render(HTMX_ELEMENT(node));
	htmx_response_set_content(response, markup);
	htmx_response_set_content_type(response, "text/html; charset=utf-8");
	htmx_response_add_header(response, "Vary", "HX-Request, HX-History-Restore-Request");
	g_object_unref(node);
	return response;
}

static gint64
oi(
	JsonObject  *object,
	const gchar *member
){
	return object != NULL ? json_object_get_int_member_with_default(object, member, 0) : 0;
}

static const gchar *
os(
	JsonObject  *object,
	const gchar *member
){
	return object != NULL ? json_object_get_string_member_with_default(object, member, "") : "";
}

static gboolean
ob(
	JsonObject  *object,
	const gchar *member
){
	return object != NULL && json_object_get_boolean_member_with_default(object, member, FALSE);
}

static gboolean
has(
	JsonObject  *object,
	const gchar *member
){
	return object != NULL && json_object_has_member(object, member) &&
		!json_node_is_null(json_object_get_member(object, member));
}

static HtmxNode *
bdi_text(const gchar *text)
{
	HtmxBdi *bdi = htmx_bdi_new();

	htmx_node_set_text_content(HTMX_NODE(bdi), text);
	return HTMX_NODE(bdi);
}

static gchar *
size_text(gint64 bytes)
{
	return axis_web_browser_format_size((guint64)MAX(bytes, 0));
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

static HtmxNode *
size_cell(gint64 bytes)
{
	g_autofree gchar *text = size_text(bytes);

	return text_cell("num", text);
}

/* "about X (Y to Z)...": the estimate, and its range on a line under it. */
static HtmxNode *
range_text_cell(const gchar *text)
{
	const gchar *open = strstr(text, " (");
	const gchar *close = open != NULL ? strchr(open, ')') : NULL;
	HtmxTd *cell;
	HtmxSpan *detail;
	g_autofree gchar *estimate = NULL;
	g_autofree gchar *head = NULL;
	g_autofree gchar *range = NULL;

	if (open == NULL || close == NULL)
		return text_cell("num", text);
	cell = htmx_td_new();
	/* What follows the range (a share: ", 70 %") stays with the estimate. */
	estimate = g_strndup(text, (gsize)(open - text));
	head = g_strconcat(estimate, close + 1, NULL);
	range = g_strndup(open + 1, (gsize)(close - open));
	htmx_node_set_text_content(HTMX_NODE(cell), head);
	detail = htmx_span_new();
	htmx_element_add_class(HTMX_ELEMENT(detail), "range-detail");
	htmx_node_set_text_content(HTMX_NODE(detail), range);
	BROWSER_ADD(cell, detail);
	return HTMX_NODE(cell);
}

static HtmxNode *
range_cell(
	gint64 mid,
	gint64 low,
	gint64 high
){
	g_autofree gchar *text = axis_web_savings_range(mid, low, high);

	return range_text_cell(text);
}

/* The saving of a figure (now - after), as a range with its share. */
static gchar *
saving_text(
	gint64 now,
	gint64 low,
	gint64 mid,
	gint64 high
){
	g_autofree gchar *range = axis_web_savings_range(now - mid, now - high, now - low);

	if (now <= 0)
		return g_strdup("nothing");
	return g_strdup_printf("%s, %" G_GINT64_FORMAT "%%", range, (now - mid) * 100 / now);
}

static HtmxNode *
saving_cell(const AxisEstimateFact *fact)
{
	g_autofree gchar *text = saving_text(fact->bytes, fact->low, fact->mid, fact->high);

	return range_text_cell(text);
}

/* The revision of @target a snapshot's figures were computed for (0 when
 * it has none). */
static gint
snapshot_revision(
	const AxisEstimateSnapshot *snapshot,
	const gchar                *target
){
	guint i;

	for (i = 0; snapshot != NULL && i < snapshot->facts->len; i++) {
		const AxisEstimateFact *fact = g_ptr_array_index(snapshot->facts, i);

		if (g_strcmp0(fact->target, target) == 0)
			return fact->revision;
	}
	return 0;
}

/* "/library/files?..." for the files of @target (the revision its figures
 * were computed for, so that the list decides with the same definition)
 * with @outcome and the filter's other conditions; %NULL when the list
 * would refuse it. */
static gchar *
list_href_at(
	const gchar       *target,
	gint               revision,
	const gchar       *outcome,
	AxisLibraryFilter *filter
){
	AxisLibraryFilter local;
	gchar *href;

	if (filter == NULL) {
		axis_library_filter_init(&local);
		filter = &local;
	}
	g_free(filter->target);
	filter->target = axis_profile_slug_valid(target) ? g_strdup(target) : NULL;
	filter->revision = revision > 0 ? revision : 0;
	filter->outcome = NULL;
	if (outcome != NULL) {
		const gchar *const *outcomes = axis_library_filter_outcomes();
		guint i;

		for (i = 0; outcomes[i] != NULL; i++) {
			if (strcmp(outcomes[i], outcome) == 0)
				filter->outcome = outcomes[i];
		}
	}
	href = filter->target != NULL ? axis_web_analysis_files_href(filter) : NULL;
	if (filter == &local)
		axis_library_filter_clear(&local);
	return href;
}

/* As list_href_at(), at the revision @snapshot's figures of @target are of. */
static gchar *
list_href(
	const AxisEstimateSnapshot *snapshot,
	const gchar                *target,
	const gchar                *outcome,
	AxisLibraryFilter          *filter
){
	return list_href_at(target, snapshot_revision(snapshot, target), outcome, filter);
}

static HtmxNode *
count_cell(
	gint64       count,
	const gchar *href
){
	g_autofree gchar *text = axis_web_browser_format_count((guint64)MAX(count, 0));
	HtmxTd *cell = htmx_td_new();

	htmx_element_add_class(HTMX_ELEMENT(cell), "num");
	BROWSER_ADD(cell, axis_web_analysis_figure(text, href));
	return HTMX_NODE(cell);
}

static AxisCalibrationState
state_from(const gchar *text)
{
	if (g_strcmp0(text, "full") == 0)
		return AXIS_CALIBRATION_FULL;
	if (g_strcmp0(text, "partly") == 0)
		return AXIS_CALIBRATION_PARTLY;
	return AXIS_CALIBRATION_NONE;
}

static HtmxNode *
calibration_badge(
	const gchar *state,
	gint64       samples
){
	AxisCalibrationState parsed = state_from(state);
	g_autofree gchar *text = samples > 0 ?
		g_strdup_printf("%s (%" G_GINT64_FORMAT " sample%s)", axis_calibration_state_label(parsed), samples,
			samples == 1 ? "" : "s") :
		g_strdup(axis_calibration_state_label(parsed));

	/* Uncalibrated is a warning, not a failure. */
	return axis_web_browser_badge(text, parsed == AXIS_CALIBRATION_FULL ? NULL : "badge-warn");
}

static HtmxNode *
away_badge(void)
{
	/* A volume that is away is offline, the same state the Storage pages show. */
	return axis_web_browser_badge("Disk offline: last known", "state-offline");
}

static JsonObject *
target_config(
	const AxisEstimateSnapshot *snapshot,
	const gchar                *id
){
	JsonObject *targets;

	if (snapshot == NULL || !json_object_has_member(snapshot->config, "targets"))
		return NULL;
	targets = json_object_get_object_member(snapshot->config, "targets");
	return targets != NULL && json_object_has_member(targets, id) ? json_object_get_object_member(targets, id) : NULL;
}

/* ── Navigation and headers ─────────────────────────────────────── */

/* The overview, each built-in profile, the profile @extra (another one
 * being shown, by its slug, @extra_label its name), the calibration. */
static HtmxNode *
tabs(
	const gchar *current,
	const gchar *extra,
	const gchar *extra_label
){
	const AxisProfileBuiltin *builtins = axis_profile_builtins();
	HtmxNav *nav = htmx_nav_new();
	HtmxUl *list = htmx_ul_new();
	guint n = AXIS_PROFILE_N_BUILTINS + (extra != NULL ? 1 : 0);
	guint i;

	htmx_element_set_attribute(HTMX_ELEMENT(nav), "aria-label", "Space savings pages");
	htmx_element_add_class(HTMX_ELEMENT(list), "tabs");
	for (i = 0; i < n + 2; i++) {
		g_autofree gchar *path = NULL;
		g_autofree gchar *label = NULL;
		HtmxLi *item = htmx_li_new();

		if (i == 0) {
			path = g_strdup("/savings");
			label = g_strdup("Overview");
		} else if (i <= AXIS_PROFILE_N_BUILTINS) {
			path = g_strdup_printf("/savings/target/%s", builtins[i - 1].slug);
			label = axis_web_analysis_profile_label(builtins[i - 1].family, builtins[i - 1].name);
		} else if (i <= n) {
			path = g_strdup_printf("/savings/target/%s", extra);
			label = g_strdup(extra_label);
		} else {
			path = g_strdup("/savings/calibration");
			label = g_strdup("Accuracy");
		}
		if (g_strcmp0(current, path) == 0) {
			HtmxSpan *here = htmx_span_new();

			htmx_element_set_attribute(HTMX_ELEMENT(here), "aria-current", "page");
			htmx_node_set_text_content(HTMX_NODE(here), label);
			BROWSER_ADD(item, here);
		} else {
			BROWSER_ADD(item, axis_web_browser_link(path, label));
		}
		BROWSER_ADD(list, item);
	}
	BROWSER_ADD(nav, list);
	return HTMX_NODE(nav);
}

static HtmxNode *
status_block(const AxisEstimateStatus *status)
{
	HtmxDiv *block = htmx_div_new();

	htmx_element_set_id(HTMX_ELEMENT(block), "savings-status");
	if (status != NULL && status->refreshing) {
		g_autofree gchar *text = g_strdup_printf("%s%s%s", status->running ? "Refreshing the estimates" :
			"A refresh is waiting for its turn", status->message != NULL && status->message[0] != '\0' ? ": " : "",
			status->message != NULL ? status->message : "");

		htmx_element_add_class(HTMX_ELEMENT(block), "notice");
		htmx_element_set_attribute(HTMX_ELEMENT(block), "role", "status");
		htmx_element_set_attribute(HTMX_ELEMENT(block), "hx-get", "/savings/status");
		htmx_element_set_attribute(HTMX_ELEMENT(block), "hx-trigger", "every 2s");
		htmx_element_set_attribute(HTMX_ELEMENT(block), "hx-swap", "outerHTML");
		if (status->running && status->progress >= 0)
			BROWSER_ADD(block, htmx_progress_new_with_value(status->progress, 1.0));
		BROWSER_ADD(block, axis_web_storage_text(NULL, text));
	}
	return HTMX_NODE(block);
}

/* The refresh: of what is refreshed on schedule, or, on a profile's page,
 * with that profile too (@slug, nullable). */
static HtmxNode *
refresh_form(
	HtmxRequest *request,
	const gchar *css,
	const gchar *slug
){
	g_autofree gchar *action = slug != NULL ? g_strdup_printf("/savings/target/%s/refresh", slug) :
		g_strdup("/savings/refresh");
	HtmxNode *form = axis_web_auth_post_form(request, action);

	htmx_element_add_class(HTMX_ELEMENT(form), "inline-form");
	axis_web_auth_add(form, axis_web_auth_button(slug != NULL ? "Refresh the estimates for this profile" :
			"Refresh the estimates now",
		css));
	return form;
}

static HtmxNode *
snapshot_header(
	HtmxRequest                *request,
	const AxisEstimateSnapshot *snapshot,
	const AxisEstimateStatus   *status,
	const gchar                *slug
){
	HtmxDiv *box = htmx_div_new();
	g_autofree gchar *when = axis_web_storage_format_time(snapshot->taken_us, "unknown");
	g_autofree gchar *line = g_strdup_printf("Estimates as of %s, computed from the index in %.1f s (model "
		"version %" G_GINT64_FORMAT "). They describe the library at that moment; a file's own page estimates "
		"it as it is now.", when, snapshot->took_ms / 1000.0, oi(snapshot->config, "model_version"));

	htmx_element_add_class(HTMX_ELEMENT(box), "snapshot-header");
	BROWSER_ADD(box, axis_web_storage_text("snapshot-time", line));
	BROWSER_ADD(box, status_block(status));
	if (axis_web_request_is_admin(request))
		BROWSER_ADD(box, refresh_form(request, NULL, slug));
	return HTMX_NODE(box);
}

static HtmxNode *
empty_card(
	HtmxRequest              *request,
	const AxisEstimateStatus *status,
	const AxisProfileRevision *profile
){
	HtmxNode *card = axis_web_storage_card("No estimate yet");

	if (profile != NULL && !profile->built_in)
		BROWSER_ADD(card, axis_web_storage_text(NULL, "No estimates for this conversion profile yet. The built-in "
			"profiles, and the profiles assigned to a library folder, are refreshed after each refresh of the Disk usage "
			"reports; any other profile's estimates are made when an administrator asks for them."));
	else
		BROWSER_ADD(card, axis_web_storage_text(NULL, "The estimates are computed after each refresh of the Disk "
			"usage reports, or when an administrator asks. None has been computed yet."));
	BROWSER_ADD(card, status_block(status));
	if (axis_web_request_is_admin(request) && (profile == NULL || profile->usable))
		BROWSER_ADD(card, refresh_form(request, "button-primary", profile != NULL && !profile->built_in ?
			profile->slug : NULL));
	return card;
}

/* The coverage block of the Analysis pages, over this snapshot's coverage
 * (the same keys). */
static HtmxNode *
coverage_card(const AxisEstimateSnapshot *snapshot)
{
	AxisAnalysisSnapshot view;

	memset(&view, 0, sizeof(view));
	view.coverage = snapshot->coverage;
	view.taken_us = snapshot->taken_us;
	return axis_web_analysis_coverage(&view, AXIS_ANALYSIS_COVER_ALL);
}

/* The notice every page carries until what it shows is calibrated, from the
 * number of its targets in each state: "uncalibrated" only when none of them
 * has a sample, "partly" as soon as one has. */
static HtmxNode *
calibration_notice(
	guint none,
	guint partly,
	guint full
){
	if (none + partly == 0)
		return NULL;
	if (partly + full == 0)
		return axis_web_browser_warning("Informed guesses only: these estimates come from how the encoders usually "
			"behave, not from conversions of your files, so the ranges are wide on purpose. Measuring a few files "
			"(Accuracy tab) or the first real conversions narrow them. Nothing here has been converted.");
	return axis_web_browser_warning("Partly based on measurements: some of these estimates are still informed "
		"guesses about how the encoders behave, so their ranges are wide on purpose. More measurements (Accuracy "
		"tab) or the first real conversions narrow them. Nothing here has been converted.");
}

/* ── Work ───────────────────────────────────────────────────────── */

typedef enum
{
	PAGE_OVERVIEW,
	PAGE_TARGET,
	PAGE_CALIBRATION
} PageId;

typedef struct
{
	PageId                page;
	gchar                *target;
	AxisEstimateSnapshot *snapshot;
	AxisEstimateStatus    status;
	AxisSampleStatus      sample;
	GHashTable           *samples;
	AxisCompatReport     *compat;	/* the verdicts, when the module is on */
	AxisProfileRevision  *current;	/* PAGE_TARGET: the profile's current revision */
	GPtrArray            *profiles;	/* AxisProfileInfo: every profile not archived */
	gboolean              fragment;
	gboolean              sample_fragment;
	GError               *error;
} PageWork;

static void
page_work_free(gpointer data)
{
	PageWork *work = data;

	g_free(work->target);
	axis_profile_revision_unref(work->current);
	g_clear_pointer(&work->profiles, g_ptr_array_unref);
	axis_estimate_snapshot_free(work->snapshot);
	axis_estimate_status_clear(&work->status);
	axis_sample_status_clear(&work->sample);
	g_clear_pointer(&work->samples, g_hash_table_unref);
	axis_compat_report_free(work->compat);
	g_clear_error(&work->error);
	g_free(work);
}

static void
page_run(
	AxisWebSavings *self,
	gpointer        data
){
	PageWork *work = data;

	if (work->sample_fragment) {
		axis_estimator_get_sample_status(self->estimator, &work->sample, &work->error);
		return;
	}
	if (!axis_estimator_get_status(self->estimator, &work->status, &work->error))
		return;
	if (work->fragment)
		return;
	if (work->page == PAGE_CALIBRATION) {
		if (!axis_estimator_get_sample_status(self->estimator, &work->sample, &work->error) ||
		    !axis_estimator_count_samples(self->estimator, &work->samples, &work->error))
			return;
	}
	/* The profiles: the one shown, or all of them. */
	{
		AxisDbPool *pool = axis_estimator_get_pool(self->estimator);
		AxisDbConn *conn = axis_db_pool_acquire(pool, &work->error);
		gboolean ok;

		if (conn == NULL)
			return;
		ok = work->page == PAGE_TARGET ? axis_profile_store_current(conn, work->target, &work->current, &work->error) :
			axis_profile_store_list(conn, FALSE, &work->profiles, &work->error);
		axis_db_pool_release(pool, conn);
		if (!ok || (work->page == PAGE_TARGET && work->current == NULL))
			return;
	}
	if (!axis_estimator_load_snapshot(self->estimator, work->page == PAGE_TARGET ? work->target : NULL,
	        &work->snapshot, &work->error))
		return;
	/* The verdicts are an addition: if they cannot be worked out the
	 * estimates still show. */
	if (self->compat != NULL && axis_compat_is_enabled(self->compat) && work->page != PAGE_CALIBRATION) {
		g_autoptr(GError) local = NULL;

		if (!axis_compat_compute_report(self->compat, &work->compat, &local))
			g_message("savings: the compatibility verdicts could not be worked out: %s", local->message);
	}
}

#define REFRESH_PLACE "savings-refresh"

/* With @notice, the notice of a refresh (@message, nullable) is drawn in a
 * block around the header, where "Refresh now" is, or at the top. */
static HtmxDiv *
page_start_notice(
	HtmxRequest    *request,
	const PageWork *work,
	const gchar    *path,
	const gchar    *title,
	AxisWebNotice  *notice,
	const gchar    *message
){
	HtmxDiv *content = htmx_div_new();
	HtmxHeading *heading = htmx_h1_new();
	gboolean other = work->current != NULL && !work->current->built_in;
	HtmxNode *header;
	HtmxDiv *block;

	htmx_node_set_text_content(HTMX_NODE(heading), title);
	BROWSER_ADD(content, heading);
	BROWSER_ADD(content, axis_web_page_intro("An estimate of how much space re-encoding your files would free, for "
		"each conversion profile, worked out before anything is changed: what would be converted, how big it would "
		"be after, and what would be skipped. Nothing is converted from this page."));
	BROWSER_ADD(content, tabs(path, other ? work->current->slug : NULL, other ? work->current->name : NULL));
	if (notice != NULL)
		axis_web_notice_begin(notice, request, FALSE, g_strdup(message), HTMX_NODE(content));
	header = work->snapshot == NULL ? empty_card(request, &work->status, work->current) :
		snapshot_header(request, work->snapshot, &work->status, other && work->current->usable ?
			work->current->slug : NULL);
	if (notice == NULL) {
		BROWSER_ADD(content, header);
		return content;
	}
	block = htmx_div_new();
	htmx_element_set_id(HTMX_ELEMENT(block), REFRESH_PLACE);
	axis_web_notice_put(notice, HTMX_NODE(block), REFRESH_PLACE);
	BROWSER_ADD(block, header);
	BROWSER_ADD(content, block);
	return content;
}

static HtmxDiv *
page_start(
	HtmxRequest    *request,
	const PageWork *work,
	const gchar    *path,
	const gchar    *title
){
	return page_start_notice(request, work, path, title, NULL, NULL);
}

/* ── Overview ───────────────────────────────────────────────────── */

static HtmxResponse *
overview_done(
	AxisWebSavings *self,
	HtmxRequest    *request,
	PageWork       *work
){
	static const AxisWebColumn columns[] = {
		{ "Conversion profile", AXIS_WEB_COL_NAME },
		{ "Files to convert", AXIS_WEB_COL_NUMBER },
		{ "Size now", AXIS_WEB_COL_SIZE },
		{ "Size after", AXIS_WEB_COL_RANGE },
		{ "Saving", AXIS_WEB_COL_RANGE },
		{ "Files skipped", AXIS_WEB_COL_COUNT },
		{ "Accuracy", AXIS_WEB_COL_STATE },
		{ NULL, 0 }
	};
	static const AxisWebColumn compat_columns[] = {
		{ "Conversion profile", AXIS_WEB_COL_NAME },
		{ "Files to convert", AXIS_WEB_COL_NUMBER },
		{ "Size now", AXIS_WEB_COL_SIZE },
		{ "Size after", AXIS_WEB_COL_RANGE },
		{ "Saving", AXIS_WEB_COL_RANGE },
		{ "Files skipped", AXIS_WEB_COL_COUNT },
		{ "Accuracy", AXIS_WEB_COL_STATE },
		{ "Plays on your devices", AXIS_WEB_COL_STATE },
		{ NULL, 0 }
	};
	g_autoptr(GPtrArray) bars = g_ptr_array_new_with_free_func((GDestroyNotify)axis_chart_bar_free);
	AxisWebNotice notice;
	g_autoptr(HtmxDiv) content = page_start_notice(request, work, "/savings", "Space savings", &notice,
		g_strcmp0(htmx_request_get_query_param(request, "done"), "refresh-requested") == 0 ?
		"A refresh has been requested; this page shows the new figures when it has run." : NULL);
	g_autoptr(GPtrArray) others = g_ptr_array_new();
	HtmxNode *others_line = NULL;
	HtmxNode *card;
	HtmxNode *table;
	HtmxTbody *body;
	guint states[3] = { 0, 0, 0 };
	guint i;

	if (work->snapshot == NULL) {
		axis_web_notice_end(&notice);
		return axis_web_page_respond(request, "Space savings", HTMX_NODE(content));
	}

	card = axis_web_storage_card("Conversion profiles side by side");
	BROWSER_ADD(card, axis_web_storage_text("help", "What converting the library with each conversion profile would "
		"save: every built-in profile, and every other profile that is assigned to a library folder or whose figures were asked "
		"for. A profile is a full combination of codec, quality, audio and subtitle handling and container; the "
		"video and the music and audiobook profiles apply to different files, so their savings do not add up."));
	table = work->compat != NULL ? axis_web_table_new("savings-profiles-compat", compat_columns) :
		axis_web_table_new("savings-profiles", columns);
	body = htmx_tbody_new();
	for (i = 0; work->profiles != NULL && i < work->profiles->len; i++) {
		const AxisProfileInfo *profile = g_ptr_array_index(work->profiles, i);
		const AxisProfileRevision *rev = profile->current;
		const gchar *slug = profile->slug;
		const AxisEstimateFact *total = axis_estimate_snapshot_fact(work->snapshot, slug, "total", "");
		JsonObject *config = target_config(work->snapshot, slug);
		const AxisProfileBuiltin *builtin = profile->built_in ? axis_profile_builtin(slug) : NULL;
		g_autofree gchar *href = g_strdup_printf("/savings/target/%s", slug);
		g_autofree gchar *name = axis_web_analysis_profile_label(builtin != NULL ? builtin->family :
			rev != NULL && rev->usable ? axis_profile_family(&rev->def) : "?", rev != NULL ? rev->name : slug);
		g_autofree gchar *eligible = NULL;
		g_autofree gchar *skipped = NULL;
		g_autofree gchar *revision = NULL;
		HtmxTr *row;
		HtmxTd *cell;
		gint64 scope = total != NULL ? oi(total->extra, "scope_paths") : 0;

		/* A profile without figures here is listed below the table. */
		if (total == NULL && !profile->built_in) {
			g_ptr_array_add(others, (gpointer)profile);
			continue;
		}
		eligible = list_href(work->snapshot, slug, "eligible", NULL);
		skipped = list_href(work->snapshot, slug, "skipped", NULL);
		row = htmx_tr_new();
		cell = htmx_td_new();
		BROWSER_ADD(cell, axis_web_browser_link(href, name));
		if (total != NULL) {
			revision = g_strdup_printf("version %d%s", total->revision, rev != NULL && rev->number != total->revision ?
				" (not the current version)" : "");
			BROWSER_ADD(cell, axis_web_storage_text("help", revision));
		}
		BROWSER_ADD(row, cell);
		if (total == NULL) {
			BROWSER_ADD(row, text_cell("num", "?"));
			BROWSER_ADD(row, text_cell("num", ""));
			BROWSER_ADD(row, text_cell("num", ""));
			BROWSER_ADD(row, text_cell("num", ""));
			BROWSER_ADD(row, text_cell("num", ""));
		} else {
			BROWSER_ADD(row, count_cell(total->paths, eligible));
			BROWSER_ADD(row, size_cell(total->bytes));
			BROWSER_ADD(row, range_cell(total->mid, total->low, total->high));
			BROWSER_ADD(row, saving_cell(total));
			BROWSER_ADD(row, count_cell(scope - total->paths, skipped));
			{
				g_autofree gchar *saving = size_text(total->bytes - total->mid);

				g_ptr_array_add(bars, axis_chart_bar_new(name, total->bytes - total->mid, saving, href, FALSE));
			}
		}
		cell = htmx_td_new();
		BROWSER_ADD(cell, calibration_badge(os(config, "calibration"), oi(config, "samples")));
		BROWSER_ADD(row, cell);
		if (work->compat != NULL)
			BROWSER_ADD(row, axis_web_compat_target_cell(work->compat, slug));
		switch (state_from(os(config, "calibration"))) {
		case AXIS_CALIBRATION_FULL:
			states[2]++;
			break;
		case AXIS_CALIBRATION_PARTLY:
			states[1]++;
			break;
		default:
			states[0]++;
			break;
		}
		BROWSER_ADD(body, row);
	}
	BROWSER_ADD(table, body);
	/* The others: their figures are made when they are assigned, or asked
	 * for on their page. */
	if (others->len > 0) {
		HtmxP *line = htmx_p_new();
		HtmxSpan *head = htmx_span_new();

		others_line = HTMX_NODE(line);
		htmx_element_add_class(HTMX_ELEMENT(line), "help");
		htmx_node_set_text_content(HTMX_NODE(head), "Conversion profiles without estimates yet (made when one is "
			"assigned to a library folder, or asked for on its page): ");
		BROWSER_ADD(line, head);
		for (i = 0; i < others->len; i++) {
			const AxisProfileInfo *profile = g_ptr_array_index(others, i);
			g_autofree gchar *href = g_strdup_printf("/savings/target/%s", profile->slug);
			HtmxSpan *sep;

			if (i > 0) {
				sep = htmx_span_new();
				htmx_node_set_text_content(HTMX_NODE(sep), ", ");
				BROWSER_ADD(line, sep);
			}
			BROWSER_ADD(line, axis_web_browser_link(href, profile->current != NULL ? profile->current->name :
				profile->slug));
		}
	}
	{
		HtmxNode *calibration = calibration_notice(states[0], states[1], states[2]);

		if (calibration != NULL)
			BROWSER_ADD(content, calibration);
	}
	BROWSER_ADD(card, table);
	if (others_line != NULL)
		BROWSER_ADD(card, others_line);
	BROWSER_ADD(card, axis_web_storage_text("help", "\"Files to convert\" counts the files that would be converted, "
		"including those on a disk that is offline (counted as Axis last saw them). \"Files skipped\" counts the "
		"files the profile is for that would not be, for the reasons on its page. Each profile's estimates are for "
		"the version named under it. Ranges: before calibration, every estimate of one codec and encoder is taken "
		"to be wrong in the same direction, so the range of a total is close to the sum of its files' ranges."));
	BROWSER_ADD(content, card);

	card = axis_web_storage_card("Estimated saving by conversion profile");
	BROWSER_ADD(card, axis_web_analysis_bar_chart("Estimated saving by conversion profile (middle of the range)",
		bars));
	BROWSER_ADD(content, card);
	if (work->compat != NULL)
		BROWSER_ADD(content, axis_web_compat_recommendation_card(work->compat, axis_web_request_is_admin(request)));
	BROWSER_ADD(content, coverage_card(work->snapshot));
	axis_web_notice_end(&notice);
	return axis_web_page_respond(request, "Space savings", HTMX_NODE(content));
}

/* ── One target ─────────────────────────────────────────────────── */

static gint
compare_facts_by_saving(
	gconstpointer a,
	gconstpointer b
){
	const AxisEstimateFact *x = *(AxisEstimateFact *const *)a;
	const AxisEstimateFact *y = *(AxisEstimateFact *const *)b;
	gint64 sx = x->bytes - x->mid;
	gint64 sy = y->bytes - y->mid;

	if (sx != sy)
		return sx > sy ? -1 : 1;
	return g_strcmp0(x->dim, y->dim);
}

static gint
compare_facts_by_id(
	gconstpointer a,
	gconstpointer b
){
	const AxisEstimateFact *x = *(AxisEstimateFact *const *)a;
	const AxisEstimateFact *y = *(AxisEstimateFact *const *)b;
	gint64 nx = g_ascii_strtoll(x->dim, NULL, 10);
	gint64 ny = g_ascii_strtoll(y->dim, NULL, 10);

	return nx < ny ? -1 : nx > ny;
}

static HtmxNode *
volumes_card(
	const AxisEstimateSnapshot *snapshot,
	const gchar                *target
){
	static const AxisWebColumn columns[] = {
		{ "Disk", AXIS_WEB_COL_NAME },
		{ "State", AXIS_WEB_COL_STATE },
		{ "Free now", AXIS_WEB_COL_SIZE },
		{ "Files to convert", AXIS_WEB_COL_NUMBER },
		{ "Saving", AXIS_WEB_COL_RANGE },
		{ "Free after", AXIS_WEB_COL_RANGE },
		{ NULL, 0 }
	};
	g_autoptr(GPtrArray) volumes = axis_estimate_snapshot_facts(snapshot, target, "volume");
	HtmxNode *card = axis_web_storage_card("Projected free space per disk");
	HtmxNode *table = axis_web_table_new("savings-volumes", columns);
	HtmxTbody *body = htmx_tbody_new();
	guint i;

	g_ptr_array_sort(volumes, compare_facts_by_id);
	for (i = 0; i < volumes->len; i++) {
		const AxisEstimateFact *fact = g_ptr_array_index(volumes, i);
		AxisLibraryFilter filter;
		g_autofree gchar *href = NULL;
		gboolean usable = ob(fact->extra, "usable");
		HtmxTr *row = htmx_tr_new();
		HtmxTd *name = htmx_td_new();
		HtmxTd *state = htmx_td_new();

		axis_library_filter_init(&filter);
		filter.volume_id = g_ascii_strtoll(fact->dim, NULL, 10);
		href = list_href(snapshot, target, "eligible", &filter);
		axis_library_filter_clear(&filter);

		BROWSER_ADD(name, bdi_text(os(fact->extra, "name")));
		BROWSER_ADD(row, name);
		BROWSER_ADD(state, axis_web_browser_badge(usable ? "Online" : "Offline: last known", usable ? NULL :
			"state-offline"));
		BROWSER_ADD(row, state);
		if (has(fact->extra, "free_bytes"))
			BROWSER_ADD(row, size_cell(oi(fact->extra, "free_bytes")));
		else
			BROWSER_ADD(row, text_cell("num", "unknown"));
		BROWSER_ADD(row, count_cell(fact->paths, href));
		BROWSER_ADD(row, saving_cell(fact));
		if (has(fact->extra, "free_bytes")) {
			gint64 free_now = oi(fact->extra, "free_bytes");

			BROWSER_ADD(row, range_cell(free_now + fact->bytes - fact->mid, free_now + fact->bytes - fact->high,
				free_now + fact->bytes - fact->low));
		} else {
			BROWSER_ADD(row, text_cell("num", "unknown"));
		}
		BROWSER_ADD(body, row);
	}
	BROWSER_ADD(table, body);
	BROWSER_ADD(card, table);
	BROWSER_ADD(card, axis_web_storage_text("help", "Free after is the free space now plus the estimated saving of "
		"the files that would be converted on that disk, as a range. A disk that is offline is shown with its last "
		"known free space and files."));
	return card;
}

/* A breakdown: one row per value of a dimension, with a file list link
 * for each.  @apply sets the filter for a row's value; %NULL leaves the
 * row without a link. */
typedef void (*ApplyFunc)(AxisLibraryFilter *filter, const AxisEstimateFact *fact);

static void
apply_root(
	AxisLibraryFilter      *filter,
	const AxisEstimateFact *fact
){
	filter->root_id = g_ascii_strtoll(fact->dim, NULL, 10);
}

static void
apply_type(
	AxisLibraryFilter      *filter,
	const AxisEstimateFact *fact
){
	const gchar *const *types = axis_library_filter_types();
	guint i;

	for (i = 0; types[i] != NULL; i++) {
		if (strcmp(types[i], fact->dim) == 0)
			filter->type = types[i];
	}
}

static void
apply_res(
	AxisLibraryFilter      *filter,
	const AxisEstimateFact *fact
){
	const gchar *const *classes = axis_library_filter_resolutions();
	guint i;

	for (i = 0; classes[i] != NULL; i++) {
		if (strcmp(classes[i], fact->dim) == 0)
			filter->resolution = classes[i];
	}
}

static void
apply_vcodec(
	AxisLibraryFilter      *filter,
	const AxisEstimateFact *fact
){
	filter->video_codec = g_strdup(fact->dim);
}

static void
apply_acodec(
	AxisLibraryFilter      *filter,
	const AxisEstimateFact *fact
){
	filter->audio_codec = g_strdup(fact->dim);
}

static const gchar *
dim_label(
	const gchar            *report,
	const AxisEstimateFact *fact
){
	if (strcmp(report, "root") == 0)
		return os(fact->extra, "name");
	if (strcmp(report, "category") == 0) {
		AxisMediaType type;

		return axis_media_type_from_string(fact->dim, &type) ? axis_media_type_label(type) : fact->dim;
	}
	if (strcmp(report, "res") == 0)
		return axis_probe_resolution_label(fact->dim);
	return fact->dim;
}

static HtmxNode *
breakdown_card(
	const AxisEstimateSnapshot *snapshot,
	const gchar                *target,
	const gchar                *report,
	const gchar                *title,
	const gchar                *column,
	ApplyFunc                   apply
){
	AxisWebColumn columns[6];
	g_autoptr(GPtrArray) facts = axis_estimate_snapshot_facts(snapshot, target, report);
	HtmxNode *card;
	HtmxNode *table;
	HtmxTbody *body;
	guint i;

	if (facts->len == 0)
		return NULL;
	/* One kind for every breakdown of the page, so they line up. */
	columns[0].heading = column;
	columns[0].role = AXIS_WEB_COL_NAME;
	columns[1].heading = "Files to convert";
	columns[1].role = AXIS_WEB_COL_NUMBER;
	columns[2].heading = "Size now";
	columns[2].role = AXIS_WEB_COL_SIZE;
	columns[3].heading = "Size after";
	columns[3].role = AXIS_WEB_COL_RANGE;
	columns[4].heading = "Saving";
	columns[4].role = AXIS_WEB_COL_RANGE;
	columns[5].heading = NULL;
	columns[5].role = 0;
	g_ptr_array_sort(facts, compare_facts_by_saving);
	card = axis_web_storage_card(title);
	table = axis_web_table_new("savings-breakdown", columns);
	body = htmx_tbody_new();
	for (i = 0; i < facts->len; i++) {
		const AxisEstimateFact *fact = g_ptr_array_index(facts, i);
		AxisLibraryFilter filter;
		g_autofree gchar *href = NULL;
		HtmxTr *row;
		HtmxTd *name;

		if (fact->paths == 0)
			continue;
		row = htmx_tr_new();
		name = htmx_td_new();
		axis_library_filter_init(&filter);
		apply(&filter, fact);
		href = list_href(snapshot, target, "eligible", &filter);
		axis_library_filter_clear(&filter);

		BROWSER_ADD(name, bdi_text(dim_label(report, fact)));
		if (oi(fact->extra, "offline_paths") > 0)
			BROWSER_ADD(name, away_badge());
		BROWSER_ADD(row, name);
		BROWSER_ADD(row, count_cell(fact->paths, href));
		BROWSER_ADD(row, size_cell(fact->bytes));
		BROWSER_ADD(row, range_cell(fact->mid, fact->low, fact->high));
		BROWSER_ADD(row, saving_cell(fact));
		BROWSER_ADD(body, row);
	}
	BROWSER_ADD(table, body);
	BROWSER_ADD(card, table);
	return card;
}

static HtmxNode *
skips_card(
	const AxisEstimateSnapshot *snapshot,
	const gchar                *target
){
	static const AxisWebColumn columns[] = {
		{ "Reason", AXIS_WEB_COL_NAME },
		{ "Files", AXIS_WEB_COL_COUNT },
		{ "Size", AXIS_WEB_COL_SIZE },
		{ NULL, 0 }
	};
	HtmxNode *card = axis_web_storage_card("Why files would be skipped");
	HtmxNode *table = axis_web_table_new("breakdown", columns);
	HtmxTbody *body = htmx_tbody_new();
	guint rows = 0;
	guint i;

	/* In the fixed order of the reasons: a file is counted under the
	 * first that applies to it. */
	for (i = 1; i < AXIS_ESTIMATE_N_REASONS; i++) {
		const gchar *id = axis_estimate_reason_to_string((AxisEstimateReason)i);
		const AxisEstimateFact *fact = axis_estimate_snapshot_fact(snapshot, target, "skip", id);
		g_autofree gchar *href = NULL;
		HtmxTr *row;
		HtmxTd *reason;
		HtmxStrong *label;

		if (fact == NULL || fact->paths == 0)
			continue;
		href = list_href(snapshot, target, id, NULL);
		row = htmx_tr_new();
		reason = htmx_td_new();
		label = htmx_strong_new();
		htmx_node_set_text_content(HTMX_NODE(label), axis_estimate_reason_label((AxisEstimateReason)i));
		BROWSER_ADD(reason, label);
		BROWSER_ADD(reason, axis_web_storage_text("help", axis_estimate_reason_help((AxisEstimateReason)i)));
		if (oi(fact->extra, "offline_paths") > 0)
			BROWSER_ADD(reason, away_badge());
		BROWSER_ADD(row, reason);
		BROWSER_ADD(row, count_cell(fact->paths, href));
		BROWSER_ADD(row, size_cell(fact->bytes));
		BROWSER_ADD(body, row);
		rows++;
	}
	BROWSER_ADD(table, body);
	if (rows == 0) {
		g_object_unref(table);
		BROWSER_ADD(card, axis_web_storage_text("help", "No file this profile applies to would be skipped."));
		return card;
	}
	BROWSER_ADD(card, table);
	BROWSER_ADD(card, axis_web_storage_text("help", "Each file is counted once, under the first reason that applies "
		"to it, in this order. Sizes count a hard-linked file once."));
	return card;
}

static HtmxNode *
top_card(
	const AxisEstimateSnapshot *snapshot,
	const gchar                *target
){
	static const AxisWebColumn columns[] = {
		{ "File", AXIS_WEB_COL_NAME },
		{ "Size now", AXIS_WEB_COL_SIZE },
		{ "Size after", AXIS_WEB_COL_RANGE },
		{ "Saving", AXIS_WEB_COL_RANGE },
		{ NULL, 0 }
	};
	const AxisEstimateFact *fact = axis_estimate_snapshot_fact(snapshot, target, "top", "");
	JsonArray *items = fact != NULL && json_object_has_member(fact->extra, "items") ?
		json_object_get_array_member(fact->extra, "items") : NULL;
	HtmxNode *card = axis_web_storage_card("The files with the largest estimated saving");
	HtmxNode *table;
	HtmxTbody *body;
	guint i;

	if (items == NULL || json_array_get_length(items) == 0) {
		BROWSER_ADD(card, axis_web_storage_text("help", "No file would be converted."));
		return card;
	}
	table = axis_web_table_new("savings-top", columns);
	body = htmx_tbody_new();
	for (i = 0; i < json_array_get_length(items); i++) {
		JsonObject *item = json_array_get_object_element(items, i);
		g_autofree gchar *href = g_strdup_printf("/library/file/%" G_GINT64_FORMAT, oi(item, "id"));
		const gchar *stored = os(item, "path");
		const gchar *slash = strrchr(stored, '/');
		g_autofree gchar *saving = saving_text(oi(item, "now"), oi(item, "low"), oi(item, "mid"), oi(item, "high"));
		HtmxTr *row = htmx_tr_new();
		HtmxTd *cell = htmx_td_new();
		HtmxA *link = htmx_a_new_with_href(href);

		htmx_element_add_class(HTMX_ELEMENT(cell), "scan-path");
		BROWSER_ADD(link, axis_web_browser_name_node(slash != NULL ? slash + 1 : stored));
		BROWSER_ADD(cell, link);
		if (slash != NULL) {
			g_autofree gchar *directory = g_strndup(stored, (gsize)(slash - stored));
			HtmxDiv *dir = htmx_div_new();

			htmx_element_add_class(HTMX_ELEMENT(dir), "file-dir");
			BROWSER_ADD(dir, axis_web_browser_name_node(directory));
			BROWSER_ADD(cell, dir);
		}
		if (!ob(item, "usable"))
			BROWSER_ADD(cell, away_badge());
		BROWSER_ADD(row, cell);
		BROWSER_ADD(row, size_cell(oi(item, "now")));
		BROWSER_ADD(row, range_cell(oi(item, "mid"), oi(item, "low"), oi(item, "high")));
		BROWSER_ADD(row, range_text_cell(saving));
		BROWSER_ADD(body, row);
	}
	BROWSER_ADD(table, body);
	BROWSER_ADD(card, table);
	return card;
}

static HtmxResponse *
target_done(
	AxisWebSavings *self,
	HtmxRequest    *request,
	PageWork       *work
){
	const AxisProfileRevision *current = work->current;
	const AxisProfileBuiltin *builtin = current->built_in ? axis_profile_builtin(current->slug) : NULL;
	const gchar *id = current->slug;
	const gchar *family = builtin != NULL ? builtin->family : current->usable ? axis_profile_family(&current->def) :
		current->kind == AXIS_PROFILE_KIND_VIDEO ? "Video" : "Audio";
	g_autofree gchar *path = g_strdup_printf("/savings/target/%s", id);
	g_autofree gchar *profile_name = axis_web_analysis_profile_label(family, current->name);
	g_autofree gchar *title = g_strdup_printf("What converting with %s would save", profile_name);
	AxisWebNotice notice;
	g_autoptr(HtmxDiv) content = page_start_notice(request, work, path, title, &notice,
		g_strcmp0(htmx_request_get_query_param(request, "done"), "refresh-requested") == 0 ?
		"Estimates for this profile have been asked for; this page shows them when the refresh has run." : NULL);
	const AxisEstimateFact *total;
	JsonObject *config;
	HtmxNode *card;
	HtmxNode *piece;
	gint computed;

	if (!current->usable)
		BROWSER_ADD(content, axis_web_browser_warning(current->unusable));
	if (work->snapshot == NULL) {
		axis_web_notice_end(&notice);
		return axis_web_page_respond(request, title, HTMX_NODE(content));
	}
	config = target_config(work->snapshot, id);
	total = axis_estimate_snapshot_fact(work->snapshot, id, "total", "");
	computed = snapshot_revision(work->snapshot, id);
	{
		AxisCalibrationState state = state_from(os(config, "calibration"));

		piece = calibration_notice(state == AXIS_CALIBRATION_NONE, state == AXIS_CALIBRATION_PARTLY,
			state == AXIS_CALIBRATION_FULL);
	}
	if (piece != NULL)
		BROWSER_ADD(content, piece);

	/* Spec: Revision named. */
	{
		g_autofree gchar *line = computed == current->number ?
			g_strdup_printf("These estimates were computed for version %d of this conversion profile, its current "
				"one.", computed) :
			g_strdup_printf("These estimates were computed for version %d of this conversion profile; its current "
				"version is %d, which they do not describe.", computed, current->number);

		BROWSER_ADD(content, computed == current->number ? axis_web_storage_text("snapshot-revision", line) :
			axis_web_browser_warning(line));
	}

	card = axis_web_storage_card("What this profile does");
	if (builtin != NULL)
		BROWSER_ADD(card, axis_web_storage_text(NULL, builtin->summary));
	else if (current->note[0] != '\0')
		BROWSER_ADD(card, axis_web_storage_text(NULL, current->note));
	BROWSER_ADD(card, axis_web_storage_text("help", os(config, "description")));
	{
		g_autofree gchar *href = g_strdup_printf("/profiles/%s", id);
		g_autofree gchar *text = g_strdup_printf("Versions and library folders of %s (Conversion profiles)",
			profile_name);

		BROWSER_ADD(card, axis_web_browser_link(href, text));
	}
	{
		HtmxP *line = htmx_p_new();
		HtmxSpan *head = htmx_span_new();

		htmx_node_set_text_content(HTMX_NODE(head), "Accuracy of the estimates: ");
		BROWSER_ADD(line, head);
		BROWSER_ADD(line, calibration_badge(os(config, "calibration"), oi(config, "samples")));
		BROWSER_ADD(card, line);
	}
	BROWSER_ADD(content, card);
	if (work->compat != NULL)
		BROWSER_ADD(content, axis_web_compat_target_card(work->compat, id, axis_web_request_is_admin(request)));

	card = axis_web_storage_card("Saving across the whole library");
	if (total != NULL) {
		HtmxNode *facts = axis_web_browser_facts_new();
		g_autofree gchar *eligible = list_href(work->snapshot, id, "eligible", NULL);
		g_autofree gchar *skipped = list_href(work->snapshot, id, "skipped", NULL);
		g_autofree gchar *count = axis_web_browser_format_count((guint64)total->paths);
		g_autofree gchar *skip_count = axis_web_browser_format_count((guint64)MAX(oi(total->extra, "scope_paths") -
			total->paths, 0));
		g_autofree gchar *scope = g_strdup_printf("%" G_GINT64_FORMAT " files (%s)", oi(total->extra, "scope_paths"),
			"hard links once in the size");
		g_autofree gchar *scope_size = size_text(oi(total->extra, "scope_bytes"));
		g_autofree gchar *now = size_text(total->bytes);
		g_autofree gchar *after = axis_web_savings_range(total->mid, total->low, total->high);
		g_autofree gchar *saving = saving_text(total->bytes, total->low, total->mid, total->high);

		axis_web_browser_fact(facts, "Files it is for", scope);
		axis_web_browser_fact(facts, "Size of those files", scope_size);
		axis_web_browser_fact_node(facts, "Would be converted", axis_web_analysis_figure(count, eligible));
		axis_web_browser_fact_node(facts, "Would be skipped", axis_web_analysis_figure(skip_count, skipped));
		axis_web_browser_fact(facts, "Current size of the files to convert", now);
		axis_web_browser_fact(facts, "Size after", after);
		axis_web_browser_fact(facts, "Saving", saving);
		if (oi(total->extra, "offline_paths") > 0) {
			g_autofree gchar *away = g_strdup_printf("%" G_GINT64_FORMAT " of them, on disks that are offline, counted "
				"as Axis last saw them", oi(total->extra, "offline_paths"));

			axis_web_browser_fact(facts, "On a disk that is offline", away);
		}
		BROWSER_ADD(card, facts);
	}
	BROWSER_ADD(content, card);

	BROWSER_ADD(content, volumes_card(work->snapshot, id));
	piece = breakdown_card(work->snapshot, id, "root", "Saving by library folder", "Library folder", apply_root);
	if (piece != NULL)
		BROWSER_ADD(content, piece);
	piece = breakdown_card(work->snapshot, id, "category", "Saving by media type", "Media type", apply_type);
	if (piece != NULL)
		BROWSER_ADD(content, piece);
	if (current->kind == AXIS_PROFILE_KIND_VIDEO) {
		piece = breakdown_card(work->snapshot, id, "res", "Saving by resolution class", "Resolution", apply_res);
		if (piece != NULL)
			BROWSER_ADD(content, piece);
		piece = breakdown_card(work->snapshot, id, "vcodec", "Saving by current video codec", "Codec", apply_vcodec);
	} else {
		piece = breakdown_card(work->snapshot, id, "acodec", "Saving by current audio codec", "Codec", apply_acodec);
	}
	if (piece != NULL)
		BROWSER_ADD(content, piece);
	BROWSER_ADD(content, skips_card(work->snapshot, id));
	BROWSER_ADD(content, top_card(work->snapshot, id));
	BROWSER_ADD(content, coverage_card(work->snapshot));
	axis_web_notice_end(&notice);
	return axis_web_page_respond(request, title, HTMX_NODE(content));
}

/* ── Calibration ────────────────────────────────────────────────── */

static HtmxNode *
sample_block(
	HtmxRequest            *request,
	const AxisSampleStatus *status
){
	HtmxDiv *block = htmx_div_new();
	gboolean active = g_strcmp0(status->state, "queued") == 0 || g_strcmp0(status->state, "running") == 0;

	htmx_element_set_id(HTMX_ELEMENT(block), "sample-status");
	if (status->job_id == 0) {
		BROWSER_ADD(block, axis_web_storage_text("help", "No measurement has been made yet. An administrator can start one below."));
		return HTMX_NODE(block);
	}
	if (active) {
		g_autofree gchar *text = g_strdup_printf("%s for %s%s%s", g_strcmp0(status->state, "running") == 0 ?
			"Measuring" : "A measurement is waiting for its turn", status->target != NULL ? status->target : "?",
			status->message != NULL ? ": " : "", status->message != NULL ? status->message : "");

		htmx_element_add_class(HTMX_ELEMENT(block), "notice");
		htmx_element_set_attribute(HTMX_ELEMENT(block), "role", "status");
		htmx_element_set_attribute(HTMX_ELEMENT(block), "hx-get", "/savings/measure/status");
		htmx_element_set_attribute(HTMX_ELEMENT(block), "hx-trigger", "every 5s");
		htmx_element_set_attribute(HTMX_ELEMENT(block), "hx-swap", "outerHTML");
		if (status->progress >= 0)
			BROWSER_ADD(block, htmx_progress_new_with_value(status->progress, 1.0));
		BROWSER_ADD(block, axis_web_storage_text(NULL, text));
	} else {
		g_autofree gchar *when = axis_web_storage_format_time(status->finished_us, "unknown");
		g_autofree gchar *text = g_strdup_printf("The last measurement (%s) ended %s: %s, %" G_GINT64_FORMAT
			" sample%s recorded.", status->target != NULL ? status->target : "?", when,
			status->state != NULL ? status->state : "?", status->samples, status->samples == 1 ? "" : "s");

		BROWSER_ADD(block, axis_web_storage_text(NULL, text));
		if (status->error != NULL)
			BROWSER_ADD(block, axis_web_browser_warning(status->error));
	}
	if (active && request != NULL && axis_web_request_is_admin(request)) {
		HtmxNode *form = axis_web_auth_post_form(request, "/savings/measure/cancel");

		htmx_element_add_class(HTMX_ELEMENT(form), "inline-form");
		axis_web_auth_add(form, axis_web_auth_button("Cancel the measurement", NULL));
		BROWSER_ADD(block, form);
	}
	return HTMX_NODE(block);
}

static HtmxResponse *
calibration_done(
	AxisWebSavings *self,
	HtmxRequest    *request,
	PageWork       *work
){
	static const AxisWebColumn columns[] = {
		{ "Conversion profile", AXIS_WEB_COL_NAME },
		{ "Accuracy", AXIS_WEB_COL_STATE },
		{ "Samples it recorded", AXIS_WEB_COL_COUNT },
		{ "Samples per way of encoding", AXIS_WEB_COL_WIDE },
		{ NULL, 0 }
	};
	g_autoptr(HtmxDiv) content = page_start(request, work, "/savings/calibration",
		"Space savings: how accurate the estimates are");
	const gchar *done = htmx_request_get_query_param(request, "done");
	AxisWebNotice notice;
	HtmxNode *card;
	HtmxNode *table;
	HtmxTbody *body;
	guint i;

	axis_web_notice_begin(&notice, request, FALSE, g_strdup(g_strcmp0(done, "measure-requested") == 0 ?
		"The measurement has been requested. It runs on its own queue, one at a time, at the lowest priority." :
		g_strcmp0(done, "measure-cancelled") == 0 ? "The measurement has been cancelled." : NULL),
		HTMX_NODE(content));

	card = axis_web_storage_card("Accuracy of each conversion profile's estimates");
	BROWSER_ADD(card, axis_web_storage_text("help", "Each estimate starts as an informed guess for one way of "
		"encoding (codec, encoder, quality and picture size). Every measured excerpt, and later every real "
		"conversion, is a sample that corrects it: below 20 samples the guess and the samples are blended, from 20 "
		"on the samples alone count. A sample counts for every conversion profile that encodes the same way, "
		"whichever profile recorded it."));
	table = axis_web_table_new("calibration", columns);
	body = htmx_tbody_new();
	for (i = 0; work->profiles != NULL && i < work->profiles->len; i++) {
		const AxisProfileInfo *profile = g_ptr_array_index(work->profiles, i);
		JsonObject *config = target_config(work->snapshot, profile->slug);
		const AxisProfileBuiltin *builtin = profile->built_in ? axis_profile_builtin(profile->slug) : NULL;
		g_autofree gchar *name = axis_web_analysis_profile_label(builtin != NULL ? builtin->family :
			profile->current != NULL && profile->current->usable ? axis_profile_family(&profile->current->def) : "?",
			profile->current != NULL ? profile->current->name : profile->slug);
		const gint64 *n = work->samples != NULL ? g_hash_table_lookup(work->samples, profile->slug) : NULL;
		g_autofree gchar *count = g_strdup_printf("%" G_GINT64_FORMAT, n != NULL ? *n : 0);
		GString *cells = g_string_new(NULL);
		JsonArray *list = config != NULL && json_object_has_member(config, "cells") ?
			json_object_get_array_member(config, "cells") : NULL;
		HtmxTr *row;
		HtmxTd *cell;
		guint j;

		/* A profile without figures in these estimates has nothing to show. */
		if (config == NULL && !profile->built_in && (n == NULL || *n == 0)) {
			g_string_free(cells, TRUE);
			continue;
		}
		row = htmx_tr_new();
		cell = htmx_td_new();
		for (j = 0; list != NULL && j < json_array_get_length(list) && j < 12; j++) {
			JsonObject *c = json_array_get_object_element(list, j);

			g_string_append_printf(cells, "%s%s: %" G_GINT64_FORMAT, j > 0 ? "; " : "", os(c, "name"),
				oi(c, "samples"));
		}
		BROWSER_ADD(row, text_cell(NULL, name));
		BROWSER_ADD(cell, calibration_badge(os(config, "calibration"), oi(config, "samples")));
		BROWSER_ADD(row, cell);
		BROWSER_ADD(row, text_cell("num", count));
		BROWSER_ADD(row, text_cell(NULL, cells->len > 0 ? cells->str : "none yet"));
		g_string_free(cells, TRUE);
		BROWSER_ADD(body, row);
	}
	BROWSER_ADD(table, body);
	BROWSER_ADD(card, table);
	BROWSER_ADD(content, card);

	card = axis_web_storage_card("Measure a profile on a few of your files");
	htmx_element_set_id(HTMX_ELEMENT(card), "measure-samples");
	axis_web_notice_put(&notice, card, "measure-samples");
	BROWSER_ADD(card, axis_web_storage_text(NULL, "A measurement encodes three short excerpts (at a quarter, half "
		"and three quarters) of a few files of each picture size the profile would convert, with the profile's "
		"encoder and quality, and records what came out as samples. It only reads the files: the encoder writes to "
		"a pipe, never to a disk, and its scratch directory is outside every disk and emptied afterwards."));
	BROWSER_ADD(card, axis_web_storage_text("help", "It is slow on purpose: one at a time, at the lowest priority. "
		"On a slow processor without hardware encoding a single 4K excerpt can take many minutes and a whole "
		"measurement hours; its progress below says how long the rest will take at the speed so far."));
	BROWSER_ADD(card, sample_block(request, &work->sample));
	if (axis_web_request_is_admin(request)) {
		HtmxNode *form = axis_web_auth_post_form(request, "/savings/measure");
		HtmxDiv *field = htmx_div_new();
		HtmxLabel *label = htmx_label_new_for("measure-target");
		HtmxSelect *select = htmx_select_new_with_name("target");

		htmx_element_add_class(HTMX_ELEMENT(form), "inline-form");
		htmx_element_add_class(HTMX_ELEMENT(field), "field");
		htmx_node_set_text_content(HTMX_NODE(label), "Conversion profile to measure");
		htmx_element_set_id(HTMX_ELEMENT(select), "measure-target");
		for (i = 0; work->profiles != NULL && i < work->profiles->len; i++) {
			const AxisProfileInfo *profile = g_ptr_array_index(work->profiles, i);
			const AxisProfileBuiltin *builtin = profile->built_in ? axis_profile_builtin(profile->slug) : NULL;
			g_autofree gchar *name = NULL;

			if (profile->current == NULL || !profile->current->usable)
				continue;
			name = axis_web_analysis_profile_label(builtin != NULL ? builtin->family :
				axis_profile_family(&profile->current->def), profile->current->name);
			/* The value is the slug (fixed characters); the name is text. */
			BROWSER_ADD(select, htmx_option_new_with_value(profile->slug, name));
		}
		BROWSER_ADD(field, label);
		BROWSER_ADD(field, select);
		axis_web_auth_add(form, field);
		axis_web_auth_add(form, axis_web_auth_button("Measure the chosen profile", NULL));
		BROWSER_ADD(card, form);
	}
	BROWSER_ADD(content, card);
	axis_web_notice_end(&notice);
	return axis_web_page_respond(request, "Space savings: how accurate the estimates are", HTMX_NODE(content));
}

/* ── Handlers ───────────────────────────────────────────────────── */

static HtmxResponse *
page_done(
	AxisWebSavings *self,
	HtmxRequest    *request,
	gpointer        data
){
	PageWork *work = data;

	if (work->error != NULL) {
		if (g_error_matches(work->error, AXIS_DB_ERROR, AXIS_DB_ERROR_TIMEOUT))
			return axis_web_error_respond(request, 503, "Too slow",
				"The estimates could not be read in time. Try again in a moment.");
		return axis_web_internal_error(request, work->error);
	}
	if (work->sample_fragment) {
		HtmxResponse *response = fragment_response(sample_block(NULL, &work->sample));

		if (g_strcmp0(work->sample.state, "queued") != 0 && g_strcmp0(work->sample.state, "running") != 0)
			htmx_response_add_header(response, "HX-Refresh", "true");
		return response;
	}
	if (work->fragment) {
		HtmxResponse *response = fragment_response(status_block(&work->status));

		if (!work->status.refreshing)
			htmx_response_add_header(response, "HX-Refresh", "true");
		return response;
	}
	switch (work->page) {
	case PAGE_TARGET:
		if (work->current == NULL)
			return not_found(request);
		return target_done(self, request, work);
	case PAGE_CALIBRATION:
		return calibration_done(self, request, work);
	default:
		return overview_done(self, request, work);
	}
}

/* Only the notice after a request is a parameter the pages take: "done",
 * with one of the values @allowed lists (space-separated), and the place it
 * was made from ("at"). */
static gboolean
query_ok(
	HtmxRequest *request,
	const gchar *allowed
){
	GHashTable *query = htmx_request_get_query_params(request);
	const gchar *done;
	g_auto(GStrv) values = NULL;

	if (query == NULL || g_hash_table_size(query) == 0)
		return TRUE;
	if (allowed == NULL || g_hash_table_size(query) != (axis_web_place_of(request) != NULL ? 2 : 1))
		return FALSE;
	done = htmx_request_get_query_param(request, "done");
	if (done == NULL)
		return FALSE;
	values = g_strsplit(allowed, " ", -1);
	return g_strv_contains((const gchar *const *)values, done);
}

static HtmxResponse *
page_handle(
	HtmxRequest *request,
	gpointer     user_data,
	PageId       page,
	const gchar *target,
	gboolean     fragment,
	gboolean     sample_fragment
){
	PageWork *work;

	if (fragment || sample_fragment) {
		if (!query_ok(request, NULL))
			return bad_request(request);
		if (!htmx_request_is_htmx(request))
			return axis_web_auth_redirect(request, sample_fragment ? "/savings/calibration" : "/savings");
	} else if (!query_ok(request, page == PAGE_OVERVIEW || page == PAGE_TARGET ? "refresh-requested" :
	                     page == PAGE_CALIBRATION ? "measure-requested measure-cancelled" : NULL)) {
		return bad_request(request);
	}

	work = g_new0(PageWork, 1);
	work->page = page;
	work->target = g_strdup(target);
	work->fragment = fragment;
	work->sample_fragment = sample_fragment;
	return axis_web_savings_dispatch(user_data, request, page_run, page_done, work, page_work_free);
}

static HtmxResponse *
handle_overview(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	return page_handle(request, user_data, PAGE_OVERVIEW, NULL, FALSE, FALSE);
}

static HtmxResponse *
handle_target(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	const gchar *id = params != NULL ? g_hash_table_lookup(params, "id") : NULL;

	/* Any profile: whether there is one of that slug is the worker's to
	 * find out (404 when not). */
	if (!axis_profile_slug_valid(id))
		return not_found(request);
	return page_handle(request, user_data, PAGE_TARGET, id, FALSE, FALSE);
}

static HtmxResponse *
handle_calibration(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	return page_handle(request, user_data, PAGE_CALIBRATION, NULL, FALSE, FALSE);
}

static HtmxResponse *
handle_status(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	return page_handle(request, user_data, PAGE_OVERVIEW, NULL, TRUE, FALSE);
}

static HtmxResponse *
handle_measure_status(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	return page_handle(request, user_data, PAGE_CALIBRATION, NULL, FALSE, TRUE);
}

/* ── Actions ────────────────────────────────────────────────────── */

typedef enum
{
	ACTION_REFRESH,
	ACTION_MEASURE,
	ACTION_CANCEL,
	ACTION_PROFILE_REFRESH
} ActionId;

typedef struct
{
	ActionId  action;
	gchar    *target;
	gchar    *user;
	gint64    job_id;
	gboolean  changed;
	GError   *error;
} ActionWork;

static void
action_work_free(gpointer data)
{
	ActionWork *work = data;

	g_free(work->target);
	g_free(work->user);
	g_clear_error(&work->error);
	g_free(work);
}

static void
action_run(
	AxisWebSavings *self,
	gpointer        data
){
	ActionWork *work = data;

	switch (work->action) {
	case ACTION_MEASURE:
		axis_estimator_request_sample(self->estimator, work->target, work->user, &work->job_id, &work->changed,
			&work->error);
		break;
	case ACTION_CANCEL:
		axis_estimator_cancel_sample(self->estimator, &work->changed, &work->error);
		break;
	case ACTION_PROFILE_REFRESH:
		axis_estimator_request_profile_refresh(self->estimator, work->target, work->user, &work->job_id,
			&work->error);
		break;
	default:
		axis_estimator_request_refresh(self->estimator, &work->job_id, &work->error);
		break;
	}
}

static HtmxResponse *
action_done(
	AxisWebSavings *self,
	HtmxRequest    *request,
	gpointer        data
){
	ActionWork *work = data;

	/* A profile a form names that is not there is a bad form; a page's
	 * profile that is not there is not found. */
	if (work->error != NULL && g_error_matches(work->error, AXIS_ESTIMATE_ERROR, AXIS_ESTIMATE_ERROR_NOT_FOUND))
		return work->action == ACTION_MEASURE ?
			axis_web_error_respond(request, 400, "Bad request", "Choose one of the profiles.") :
			axis_web_error_respond(request, 404, "Not found", "There is no such profile, or it cannot be estimated.");
	if (work->error != NULL && g_error_matches(work->error, AXIS_ESTIMATE_ERROR, AXIS_ESTIMATE_ERROR_FAILED))
		return axis_web_error_respond(request, 409, "Not usable", "The profile's definition is not usable, so it "
			"cannot be estimated or measured.");
	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	switch (work->action) {
	case ACTION_PROFILE_REFRESH: {
		g_autofree gchar *location = g_strdup_printf("/savings/target/%s", work->target);

		axis_web_storage_audit(request, "savings refresh requested for profile %s: job=%" G_GINT64_FORMAT,
			work->target, work->job_id);
		return axis_web_place_redirect(request, location, "refresh-requested", REFRESH_PLACE);
	}
	case ACTION_MEASURE:
		axis_web_storage_audit(request, "sample measurement requested: target=%s job=%" G_GINT64_FORMAT,
			work->target, work->job_id);
		return axis_web_place_redirect(request, "/savings/calibration", "measure-requested", "measure-samples");
	case ACTION_CANCEL:
		axis_web_storage_audit(request, "sample measurement cancelled: %s", work->changed ? "yes" : "nothing to cancel");
		return axis_web_place_redirect(request, "/savings/calibration", "measure-cancelled", "measure-samples");
	default:
		axis_web_storage_audit(request, "savings refresh requested: job=%" G_GINT64_FORMAT, work->job_id);
		return axis_web_place_redirect(request, "/savings", "refresh-requested", REFRESH_PLACE);
	}
}

static HtmxResponse *
handle_refresh(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	ActionWork *work = g_new0(ActionWork, 1);

	work->action = ACTION_REFRESH;
	return axis_web_savings_dispatch(user_data, request, action_run, action_done, work, action_work_free);
}

static HtmxResponse *
handle_measure(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	const gchar *id = htmx_request_get_form_value(request, "target");
	ActionWork *work;

	if (!axis_profile_slug_valid(id))
		return axis_web_error_respond(request, 400, "Bad request", "Choose one of the profiles.");
	work = g_new0(ActionWork, 1);
	work->action = ACTION_MEASURE;
	work->target = g_strdup(id);
	work->user = g_strdup(axis_web_request_get_username(request));
	return axis_web_savings_dispatch(user_data, request, action_run, action_done, work, action_work_free);
}

static HtmxResponse *
handle_target_refresh(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	const gchar *id = params != NULL ? g_hash_table_lookup(params, "id") : NULL;
	ActionWork *work;

	if (!axis_profile_slug_valid(id))
		return not_found(request);
	work = g_new0(ActionWork, 1);
	work->action = ACTION_PROFILE_REFRESH;
	work->target = g_strdup(id);
	work->user = g_strdup(axis_web_request_get_username(request));
	return axis_web_savings_dispatch(user_data, request, action_run, action_done, work, action_work_free);
}

static HtmxResponse *
handle_cancel(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	ActionWork *work = g_new0(ActionWork, 1);

	work->action = ACTION_CANCEL;
	return axis_web_savings_dispatch(user_data, request, action_run, action_done, work, action_work_free);
}

/* ── The dashboard: what converting would save ──────────────────── */

typedef struct
{
	AxisEstimateSnapshot *snapshot;
	GPtrArray            *profiles;	/* AxisProfileInfo */
} DashFigures;

static void
dash_figures_free(gpointer data)
{
	DashFigures *figures = data;

	axis_estimate_snapshot_free(figures->snapshot);
	g_clear_pointer(&figures->profiles, g_ptr_array_unref);
	g_free(figures);
}

static gpointer
dash_collect(gpointer user_data)
{
	AxisWebSavings *self = user_data;
	DashFigures *figures = g_new0(DashFigures, 1);
	AxisDbPool *pool = axis_estimator_get_pool(self->estimator);
	g_autoptr(GError) error = NULL;
	AxisDbConn *conn = axis_db_pool_acquire(pool, &error);
	gboolean ok = FALSE;

	if (conn != NULL) {
		ok = axis_profile_store_list(conn, FALSE, &figures->profiles, &error);
		axis_db_pool_release(pool, conn);
	}
	if (ok)
		ok = axis_estimator_load_snapshot(self->estimator, NULL, &figures->snapshot, &error);
	if (!ok) {
		g_debug("dashboard: cannot read the estimates: %s", error != NULL ? error->message : "?");
		dash_figures_free(figures);
		return NULL;
	}
	return figures;
}

/* The profile that would save most, as the overview lists it: its saving
 * with the range and its share, how calibrated that is, and when the
 * estimates were made. */
static void
dash_render(
	AxisWebDashboard *board,
	HtmxRequest      *request,
	gpointer          data,
	gpointer          user_data
){
	DashFigures *figures = data;
	HtmxDiv *piece = htmx_div_new();
	const AxisProfileInfo *best = NULL;
	const AxisEstimateFact *best_total = NULL;
	guint i;

	htmx_element_add_class(HTMX_ELEMENT(piece), "dash-piece");
	if (figures == NULL || figures->snapshot == NULL) {
		BROWSER_ADD(piece, axis_web_storage_text("help", figures == NULL ? "The estimates could not be read just now. "
			"Reload the page in a moment." :
			"Nothing has been estimated yet: the estimates follow the Disk usage reports."));
		axis_web_dashboard_add(board, AXIS_WEB_DASHBOARD_SAVINGS, 0, "/savings", HTMX_NODE(piece));
		return;
	}
	for (i = 0; figures->profiles != NULL && i < figures->profiles->len; i++) {
		const AxisProfileInfo *profile = g_ptr_array_index(figures->profiles, i);
		const AxisEstimateFact *total = axis_estimate_snapshot_fact(figures->snapshot, profile->slug, "total", "");

		if (total == NULL || profile->current == NULL)
			continue;
		if (best_total == NULL || total->bytes - total->mid > best_total->bytes - best_total->mid) {
			best = profile;
			best_total = total;
		}
	}

	if (best == NULL) {
		BROWSER_ADD(piece, axis_web_storage_text("help", "No conversion profile has estimates yet."));
	} else {
		const AxisProfileRevision *rev = best->current;
		const AxisProfileBuiltin *builtin = best->built_in ? axis_profile_builtin(best->slug) : NULL;
		JsonObject *config = target_config(figures->snapshot, best->slug);
		g_autofree gchar *href = g_strdup_printf("/savings/target/%s", best->slug);
		g_autofree gchar *name = axis_web_analysis_profile_label(builtin != NULL ? builtin->family :
			rev->usable ? axis_profile_family(&rev->def) : "?", rev->name);
		g_autofree gchar *saving = saving_text(best_total->bytes, best_total->low, best_total->mid, best_total->high);
		HtmxDiv *figure = htmx_div_new();
		HtmxSpan *value = htmx_span_new();
		HtmxP *line = htmx_p_new();
		HtmxSpan *label = htmx_span_new();

		htmx_element_add_class(HTMX_ELEMENT(figure), "figure");
		htmx_element_add_class(HTMX_ELEMENT(figure), "figure-long");
		htmx_element_add_class(HTMX_ELEMENT(value), "figure-value");
		htmx_node_set_text_content(HTMX_NODE(value), saving);
		htmx_element_add_class(HTMX_ELEMENT(label), "figure-label");
		htmx_node_set_text_content(HTMX_NODE(label), "the estimated saving of the conversion profile that would save most");
		BROWSER_ADD(figure, value);
		BROWSER_ADD(figure, label);
		BROWSER_ADD(piece, figure);
		htmx_element_add_class(HTMX_ELEMENT(line), "state-line");
		BROWSER_ADD(line, axis_web_browser_link(href, name));
		BROWSER_ADD(line, calibration_badge(os(config, "calibration"), oi(config, "samples")));
		BROWSER_ADD(piece, line);
	}
	BROWSER_ADD(piece, axis_web_dashboard_as_of("Estimates as of", figures->snapshot->taken_us));
	axis_web_dashboard_add(board, AXIS_WEB_DASHBOARD_SAVINGS, 0, "/savings", HTMX_NODE(piece));
}

void
axis_web_savings_register_pages(
	AxisWebSavings *self,
	AxisWeb        *web
){
	axis_web_add_dashboard_part(web, dash_collect, dash_render, dash_figures_free, self);
	axis_web_set_dashboard_part_card(web, AXIS_WEB_DASHBOARD_SAVINGS);
	axis_web_get(web, "/savings", handle_overview, self);
	axis_web_get(web, "/savings/target/:id", handle_target, self);
	axis_web_get(web, "/savings/calibration", handle_calibration, self);
	axis_web_get(web, "/savings/status", handle_status, self);
	axis_web_get(web, "/savings/measure/status", handle_measure_status, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/savings/refresh", AXIS_WEB_ROUTE_ADMIN, handle_refresh, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/savings/target/:id/refresh", AXIS_WEB_ROUTE_ADMIN,
		handle_target_refresh, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/savings/measure", AXIS_WEB_ROUTE_ADMIN, handle_measure, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/savings/measure/cancel", AXIS_WEB_ROUTE_ADMIN, handle_cancel, self);
}
