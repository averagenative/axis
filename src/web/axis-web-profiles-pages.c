/*
 * axis-web-profiles-pages.c - the profile list, a profile's page, copy,
 * archive, and the assignment page
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Every figure comes from the latest Savings snapshot that has the profile
 * and says which revision it was computed for.  A profile's name and note
 * are text content only; links carry slugs and numbers.
 */

#include "web/axis-web-profiles-private.h"

#include <string.h>

#include "db/axis-db-conn.h"
#include "web/axis-web-compat.h"
#include "web/axis-web-notice.h"
#include "web/axis-web-place.h"
#include "web/axis-web-savings-private.h"

/* ── Pieces ─────────────────────────────────────────────────────── */

static HtmxResponse *
not_found(HtmxRequest *request)
{
	return axis_web_error_respond(request, 404, "Not found", "There is no such profile.");
}

static HtmxResponse *
bad_request(HtmxRequest *request)
{
	return axis_web_error_respond(request, 400, "Bad request", "The address is not valid.");
}

static gchar *
size_text(gint64 bytes)
{
	return axis_web_browser_format_size((guint64)MAX(bytes, 0));
}

static gchar *
saving_text(const AxisEstimateFact *fact)
{
	g_autofree gchar *range = NULL;

	if (fact == NULL || fact->bytes <= 0)
		return g_strdup("nothing");
	range = axis_web_savings_range(fact->bytes - fact->mid, fact->bytes - fact->high, fact->bytes - fact->low);
	return g_strdup_printf("%s, %" G_GINT64_FORMAT "%%", range, (fact->bytes - fact->mid) * 100 / fact->bytes);
}

static HtmxNode *
text_cell(const gchar *text)
{
	HtmxTd *cell = htmx_td_new();

	htmx_node_set_text_content(HTMX_NODE(cell), text);
	return HTMX_NODE(cell);
}

static HtmxNode *
node_cell(HtmxNode *node)
{
	HtmxTd *cell = htmx_td_new();

	BROWSER_ADD(cell, node);
	return HTMX_NODE(cell);
}

static const gchar *
kind_label(AxisProfileKind kind)
{
	return kind == AXIS_PROFILE_KIND_AUDIO ? "Audio" : "Video";
}

/* The listed name of the profile a copy was made from, or its short name when that one is not listed. */
static gchar *
profile_source_label(
	GPtrArray   *profiles,
	const gchar *slug
){
	guint i;

	for (i = 0; profiles != NULL && i < profiles->len; i++) {
		const AxisProfileInfo *p = g_ptr_array_index(profiles, i);

		if (g_strcmp0(p->slug, slug) == 0 && p->current != NULL)
			return axis_web_profiles_label(p->current);
	}
	return g_strdup(slug);
}

static gchar *
profile_href(const gchar *slug)
{
	return g_strdup_printf("/profiles/%s", slug);
}

/* Only "done" (one of @allowed), the place a form came back to ("at") and
 * the names in @names may be given. */
static gboolean
query_ok(
	HtmxRequest        *request,
	const gchar        *allowed,
	const gchar *const *names
){
	GHashTable *query = htmx_request_get_query_params(request);
	GHashTableIter iter;
	gpointer key;
	gpointer value;

	if (query == NULL)
		return TRUE;
	g_hash_table_iter_init(&iter, query);
	while (g_hash_table_iter_next(&iter, &key, &value)) {
		if (strcmp(key, "done") == 0) {
			g_auto(GStrv) values = allowed != NULL ? g_strsplit(allowed, " ", -1) : NULL;

			if (values == NULL || !g_strv_contains((const gchar *const *)values, value))
				return FALSE;
		} else if (strcmp(key, AXIS_WEB_PLACE_AT) == 0) {
			if (!axis_web_place_id_valid(value))
				return FALSE;
		} else if (names == NULL || !g_strv_contains(names, key)) {
			return FALSE;
		}
	}
	return TRUE;
}

static const gchar *
done_message(const gchar *done)
{
	static const struct
	{
		const gchar *key;
		const gchar *text;
	} messages[] = {
		{ "copied", "The copy has been made. It is locked, like every new version: edit it, then assign it." },
		{ "saved", "A new version has been saved; the earlier ones are kept as they were." },
		{ "unchanged", "Nothing had changed, so no version was added." },
		{ "archived", "The profile is archived: it is no longer offered for assignment, and its record is kept." },
		{ "restored", "The profile is offered again." },
		{ "assigned", "The profile is assigned. Nothing has been converted: an assignment records the intent, and "
			"the estimates of that library include it from the next refresh." },
		{ "unassigned", "The assignment has been removed. Nothing has been changed on disk." },
		{ "refresh-requested", "Figures for this profile have been asked for." }
	};
	guint i;

	for (i = 0; done != NULL && i < G_N_ELEMENTS(messages); i++) {
		if (strcmp(messages[i].key, done) == 0)
			return messages[i].text;
	}
	return NULL;
}

/* The heading, what the page is for (@intro, nullable) and what was just
 * done. */
static HtmxDiv *
page_start(
	HtmxRequest   *request,
	const gchar   *title,
	const gchar   *intro,
	AxisWebNotice *notice
){
	HtmxDiv *content = htmx_div_new();
	HtmxHeading *heading = htmx_h1_new();
	const gchar *done = done_message(htmx_request_get_query_param(request, "done"));

	htmx_node_set_text_content(HTMX_NODE(heading), title);
	BROWSER_ADD(content, heading);
	if (intro != NULL)
		BROWSER_ADD(content, axis_web_page_intro(intro));
	axis_web_notice_begin(notice, request, FALSE, g_strdup(done), HTMX_NODE(content));
	return content;
}

static const gchar *
assigned_roots(
	GPtrArray   *assignments,
	const gchar *slug,
	GString     *out
){
	guint i;

	for (i = 0; assignments != NULL && i < assignments->len; i++) {
		const AxisProfileAssignment *a = g_ptr_array_index(assignments, i);

		if (g_strcmp0(a->slug, slug) == 0)
			g_string_append_printf(out, "%s%s", out->len > 0 ? ", " : "", a->root_name);
	}
	return out->len > 0 ? out->str : "nowhere";
}

static gboolean
read_roots(
	AxisDbConn  *conn,
	GPtrArray  **roots,
	GError     **error
);

/* ── Work ───────────────────────────────────────────────────────── */

typedef enum
{
	PAGE_LIST,
	PAGE_PROFILE,
	PAGE_ASSIGN
} PageId;

typedef struct
{
	gint64  id;
	gchar  *name;
	gchar  *media_type;
} Root;

static void
root_free(Root *root)
{
	g_free(root->name);
	g_free(root->media_type);
	g_free(root);
}

typedef struct
{
	PageId                page;
	gchar                *slug;
	gboolean              archived;
	gint64                root;	/* PAGE_ASSIGN: the pairing asked about */
	gchar                *pairing;
	GPtrArray            *profiles;	/* AxisProfileInfo */
	GPtrArray            *history;	/* AxisProfileRevision, newest first */
	AxisProfileRevision  *current;
	GPtrArray            *assignments;	/* AxisProfileAssignment */
	GPtrArray            *roots;	/* Root */
	AxisEstimateSnapshot *snapshot;
	AxisCompatReport     *compat;
	GError               *error;
} PageWork;

static void
page_work_free(gpointer data)
{
	PageWork *work = data;

	g_free(work->slug);
	g_free(work->pairing);
	g_clear_pointer(&work->profiles, g_ptr_array_unref);
	g_clear_pointer(&work->history, g_ptr_array_unref);
	axis_profile_revision_unref(work->current);
	g_clear_pointer(&work->assignments, g_ptr_array_unref);
	g_clear_pointer(&work->roots, g_ptr_array_unref);
	axis_estimate_snapshot_free(work->snapshot);
	axis_compat_report_free(work->compat);
	g_clear_error(&work->error);
	g_free(work);
}

static void
page_run(
	AxisWebProfiles *self,
	gpointer         data
){
	PageWork *work = data;
	AxisDbPool *pool = axis_estimator_get_pool(self->estimator);
	AxisDbConn *conn = axis_db_pool_acquire(pool, &work->error);
	gboolean ok;
	guint i;

	if (conn == NULL)
		return;
	ok = axis_profile_store_assignments(conn, &work->assignments, &work->error);
	if (ok && work->page == PAGE_PROFILE) {
		ok = axis_profile_store_history(conn, work->slug, &work->history, &work->error);
		for (i = 0; ok && i < work->history->len; i++) {
			AxisProfileRevision *rev = g_ptr_array_index(work->history, i);

			if (rev->current)
				work->current = axis_profile_revision_ref(rev);
		}
	} else if (ok) {
		ok = axis_profile_store_list(conn, work->archived, &work->profiles, &work->error);
	}
	if (ok && work->page == PAGE_ASSIGN)
		ok = read_roots(conn, &work->roots, &work->error);
	axis_db_pool_release(pool, conn);
	if (!ok || (work->page == PAGE_PROFILE && work->current == NULL))
		return;
	if (!axis_estimator_load_snapshot(self->estimator, work->page == PAGE_PROFILE ? work->slug : NULL,
	        &work->snapshot, &work->error))
		return;
	if (work->page != PAGE_LIST)
		work->compat = axis_web_profiles_compat_report(self);
}

static gboolean
read_roots(
	AxisDbConn  *conn,
	GPtrArray  **roots,
	GError     **error
){
	g_autoptr(AxisDbResult) result = NULL;
	guint i;

	*roots = g_ptr_array_new_with_free_func((GDestroyNotify)root_free);
	if (!axis_db_conn_exec(conn, "SELECT id, name, media_type FROM library_root ORDER BY name, id", 0, NULL, &result,
	        error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		Root *root = g_new0(Root, 1);

		axis_db_result_get_int64(result, i, 0, &root->id);
		root->name = g_strdup(axis_db_result_get_text(result, i, 1));
		root->media_type = g_strdup(axis_db_result_get_text(result, i, 2));
		g_ptr_array_add(*roots, root);
	}
	return TRUE;
}

/* ── The list ───────────────────────────────────────────────────── */

static HtmxResponse *
list_done(
	AxisWebProfiles *self,
	HtmxRequest     *request,
	PageWork        *work
){
	static const AxisWebColumn columns[] = {
		{ "Profile", AXIS_WEB_COL_NAME },
		{ "Kind", AXIS_WEB_COL_TEXT },
		{ "Origin", AXIS_WEB_COL_WIDE },
		{ "Current version", AXIS_WEB_COL_NUMBER },
		{ "Locked or proven", AXIS_WEB_COL_STATE },
		{ "Assigned to", AXIS_WEB_COL_WIDE },
		{ "Estimated saving", AXIS_WEB_COL_WIDE },
		{ NULL, 0 }
	};
	AxisWebNotice notice;
	g_autoptr(HtmxDiv) content = page_start(request, "Conversion profiles", "The rules for re-encoding files you "
		"already have, to save space: which files are worth converting, and into what codec, quality and "
		"container. Not to be confused with quality profiles, which choose what to download.", &notice);
	HtmxNode *card = axis_web_storage_card("Built-in profiles and your copies");
	HtmxNode *table = axis_web_table_new("profiles", columns);
	HtmxTbody *body = htmx_tbody_new();
	guint i;

	BROWSER_ADD(content, axis_web_storage_text(NULL, "A conversion profile says which files are worth converting "
		"and what each becomes. The built-in ones cannot be changed; copy one to make your own. Every change makes "
		"a new version, and a new version is locked until it is proven: it can make trials of single files, and an "
		"administrator proves it on the Conversions page once enough trials have passed. A proven version can "
		"replace files, one at a time or in a conversion run."));
	BROWSER_ADD(content, axis_web_storage_text("help", "Whatever a profile says, some files are never converted: "
		"pinned files (hard-linked or seeding), Dolby Vision, uncertain HDR, HDR10+, interlaced or possibly "
		"interlaced video, HDR made SDR, files whose facts are missing or do not add up, and files or albums not "
		"estimated smaller by at least 5%. No profile has a setting for these."));
	for (i = 0; work->profiles != NULL && i < work->profiles->len; i++) {
		const AxisProfileInfo *profile = g_ptr_array_index(work->profiles, i);
		const AxisProfileRevision *rev = profile->current;
		const AxisEstimateFact *total = axis_estimate_snapshot_fact(work->snapshot, profile->slug, "total", "");
		g_autofree gchar *href = profile_href(profile->slug);
		g_autofree gchar *label = rev != NULL ? axis_web_profiles_label(rev) : g_strdup(profile->slug);
		g_autofree gchar *source = profile->copied_from != NULL ? profile_source_label(work->profiles,
			profile->copied_from) : NULL;
		g_autofree gchar *origin = profile->built_in ? g_strdup("built in") : source != NULL ?
			g_strdup_printf("a copy of %s", source) : g_strdup("a copy");
		g_autofree gchar *number = g_strdup_printf("%d of %u versions", rev != NULL ? rev->number : 0,
			profile->revisions);
		g_autoptr(GString) where = g_string_new(NULL);
		HtmxTr *row = htmx_tr_new();
		HtmxTd *name = htmx_td_new();

		BROWSER_ADD(name, axis_web_browser_link(href, label));
		if (profile->archived)
			BROWSER_ADD(name, axis_web_browser_badge("Archived", NULL));
		BROWSER_ADD(row, name);
		BROWSER_ADD(row, text_cell(kind_label(profile->kind)));
		BROWSER_ADD(row, text_cell(origin));
		BROWSER_ADD(row, text_cell(number));
		BROWSER_ADD(row, node_cell(rev != NULL ? axis_web_profiles_lock_badge(rev) :
			axis_web_browser_badge("Not usable", "badge-failed")));
		BROWSER_ADD(row, text_cell(assigned_roots(work->assignments, profile->slug, where)));
		if (total != NULL) {
			g_autofree gchar *saving = saving_text(total);
			g_autofree gchar *text = g_strdup_printf("%s (version %d)", saving, total->revision);

			BROWSER_ADD(row, text_cell(text));
		} else {
			BROWSER_ADD(row, text_cell("not estimated yet"));
		}
		BROWSER_ADD(body, row);
	}
	BROWSER_ADD(table, body);
	BROWSER_ADD(card, table);
	BROWSER_ADD(card, axis_web_storage_text("help", "Estimated savings are those of the latest estimates, which "
		"cover every built-in profile and every profile assigned to a library; another profile's figures are "
		"made on its Space savings page when asked for."));
	{
		HtmxDiv *links = htmx_div_new();

		htmx_element_add_class(HTMX_ELEMENT(links), "actions");
		BROWSER_ADD(links, axis_web_browser_link("/profiles/assign", "Assign conversion profiles to library folders"));
		BROWSER_ADD(links, axis_web_browser_link(work->archived ? "/profiles" : "/profiles?archived=yes",
			work->archived ? "Hide archived profiles" : "Show archived profiles"));
		BROWSER_ADD(card, links);
	}
	BROWSER_ADD(content, card);
	axis_web_notice_end(&notice);
	return axis_web_page_respond(request, "Conversion profiles", HTMX_NODE(content));
}

/* ── A profile ──────────────────────────────────────────────────── */

static HtmxNode *
definition_card(const AxisProfileRevision *rev)
{
	HtmxNode *card = axis_web_storage_card("What this profile converts, and into what");

	if (rev->usable) {
		g_autoptr(GPtrArray) facts = axis_profile_describe(&rev->def);
		HtmxNode *list = axis_web_browser_facts_new();
		guint i;

		for (i = 0; i < facts->len; i++) {
			const AxisProfileFact *f = g_ptr_array_index(facts, i);

			axis_web_browser_fact(list, f->label, f->text);
		}
		BROWSER_ADD(card, list);
	} else {
		BROWSER_ADD(card, axis_web_browser_warning(rev->unusable));
	}
	{
		HtmxDetails *details = htmx_details_new();
		HtmxSummary *summary = htmx_summary_new();
		HtmxPre *pre = htmx_pre_new();
		g_autofree gchar *hash = g_strdup_printf("SHA-256 %s", rev->hash != NULL ? rev->hash : "");

		htmx_node_set_text_content(HTMX_NODE(summary), "The definition as stored");
		htmx_element_add_class(HTMX_ELEMENT(pre), "definition-text");
		htmx_node_set_text_content(HTMX_NODE(pre), rev->text != NULL ? rev->text : "");
		BROWSER_ADD(details, summary);
		BROWSER_ADD(details, pre);
		BROWSER_ADD(details, axis_web_storage_text("help", hash));
		BROWSER_ADD(card, details);
	}
	return card;
}

static HtmxNode *
history_card(GPtrArray *history)
{
	static const AxisWebColumn columns[] = {
		{ "Version", AXIS_WEB_COL_TEXT },
		{ "Name", AXIS_WEB_COL_NAME },
		{ "Made", AXIS_WEB_COL_DATE },
		{ "By", AXIS_WEB_COL_WIDE },
		{ "Definition hash", AXIS_WEB_COL_TEXT },
		{ "Locked or proven", AXIS_WEB_COL_STATE },
		{ NULL, 0 }
	};
	HtmxNode *card = axis_web_storage_card("Versions");
	HtmxNode *table = axis_web_table_new("revisions", columns);
	HtmxTbody *body = htmx_tbody_new();
	guint i;

	for (i = 0; i < history->len; i++) {
		const AxisProfileRevision *rev = g_ptr_array_index(history, i);
		g_autofree gchar *number = g_strdup_printf("%d%s", rev->number, rev->current ? " (current)" : "");
		g_autofree gchar *when = axis_web_storage_format_time(rev->created_us, "unknown");
		g_autofree gchar *hash = g_strndup(rev->hash != NULL ? rev->hash : "", 12);
		HtmxTr *row = htmx_tr_new();

		BROWSER_ADD(row, text_cell(number));
		BROWSER_ADD(row, text_cell(rev->name));
		BROWSER_ADD(row, text_cell(when));
		BROWSER_ADD(row, text_cell(g_strcmp0(rev->origin, "code") == 0 ? "built in" :
			rev->created_by != NULL ? rev->created_by : "unknown"));
		BROWSER_ADD(row, text_cell(rev->usable ? hash : "not usable"));
		BROWSER_ADD(row, node_cell(axis_web_profiles_lock_badge(rev)));
		BROWSER_ADD(body, row);
	}
	BROWSER_ADD(table, body);
	BROWSER_ADD(card, table);
	BROWSER_ADD(card, axis_web_storage_text("help", "Versions are never changed or deleted: an edit adds one. "
		"Estimates, samples and assignments name the version they were made with."));
	return card;
}

static HtmxNode *
savings_card(
	HtmxRequest                *request,
	const AxisProfileRevision  *rev,
	const AxisEstimateSnapshot *snapshot
){
	HtmxNode *card = axis_web_storage_card("Estimated saving");
	const AxisEstimateFact *total = axis_estimate_snapshot_fact(snapshot, rev->slug, "total", "");
	g_autofree gchar *href = g_strdup_printf("/savings/target/%s", rev->slug);

	if (total == NULL) {
		BROWSER_ADD(card, axis_web_storage_text(NULL, rev->built_in ? "No estimate has been made yet: built-in profiles are estimated at the next refresh on the "
			"Space savings page." :
			"No figures for this profile yet: they are made when it is assigned to a library, or when an "
			"administrator asks for them on its Space savings page."));
	} else {
		HtmxNode *facts = axis_web_browser_facts_new();
		g_autofree gchar *when = axis_web_storage_format_time(snapshot->taken_us, "unknown");
		g_autofree gchar *count = axis_web_browser_format_count((guint64)total->paths);
		g_autofree gchar *saving = saving_text(total);
		g_autofree gchar *now = size_text(total->bytes);
		g_autofree gchar *revision = g_strdup_printf("version %d%s", total->revision, total->revision == rev->number ?
			" (the current one)" : " (not the current one: the figures do not describe it)");

		axis_web_browser_fact(facts, "Computed", when);
		axis_web_browser_fact(facts, "Estimated for", revision);
		axis_web_browser_fact(facts, "Files that would be converted", count);
		axis_web_browser_fact(facts, "Size of those files now", now);
		axis_web_browser_fact(facts, "Saving", saving);
		BROWSER_ADD(card, facts);
	}
	BROWSER_ADD(card, axis_web_browser_link(href, "Space savings for this profile, every figure linked to its files"));
	(void)request;
	return card;
}

static HtmxNode *
actions_card(
	HtmxRequest               *request,
	const AxisProfileRevision *rev,
	AxisWebNotice             *notice
){
	HtmxNode *card = axis_web_storage_card("Copy, edit or archive this profile");
	g_autofree gchar *copy = g_strdup_printf("/profiles/%s/copy", rev->slug);
	g_autofree gchar *edit = g_strdup_printf("/profiles/%s/edit", rev->slug);
	g_autofree gchar *archive = g_strdup_printf("/profiles/%s/%s", rev->slug, rev->archived ? "restore" : "archive");

	htmx_element_set_id(HTMX_ELEMENT(card), "change-it");
	axis_web_notice_put(notice, card, "change-it");
	if (rev->usable) {
		HtmxNode *form = axis_web_auth_post_form(request, copy);
		HtmxDiv *field = htmx_div_new();
		HtmxLabel *label = htmx_label_new_for("copy-name");
		HtmxInput *input = htmx_input_new(HTMX_INPUT_TEXT);

		htmx_element_add_class(HTMX_ELEMENT(form), "form-wide");
		htmx_element_add_class(HTMX_ELEMENT(field), "field");
		htmx_node_set_text_content(HTMX_NODE(label), "Name of the copy (empty: \"Copy of\" this one's name)");
		htmx_input_set_name(input, "name");
		htmx_element_set_id(HTMX_ELEMENT(input), "copy-name");
		htmx_element_set_attribute(HTMX_ELEMENT(input), "maxlength", "80");
		htmx_element_set_attribute(HTMX_ELEMENT(input), "autocomplete", "off");
		BROWSER_ADD(field, label);
		BROWSER_ADD(field, input);
		axis_web_auth_add(form, field);
		axis_web_auth_add(form, axis_web_auth_button("Copy this profile", NULL));
		BROWSER_ADD(card, form);
	}
	if (rev->built_in) {
		BROWSER_ADD(card, axis_web_storage_text("help", "A built-in profile cannot be edited or archived: copy it and "
			"edit the copy."));
	} else {
		HtmxDiv *row = htmx_div_new();
		HtmxNode *form = axis_web_auth_post_form(request, archive);

		htmx_element_add_class(HTMX_ELEMENT(row), "actions");
		if (rev->usable && !rev->archived)
			BROWSER_ADD(row, axis_web_browser_link(edit, "Edit this profile"));
		htmx_element_add_class(HTMX_ELEMENT(form), "inline-form");
		axis_web_auth_add(form, axis_web_auth_button(rev->archived ? "Offer this profile again" : "Archive this profile", NULL));
		BROWSER_ADD(row, form);
		BROWSER_ADD(card, row);
	}
	if (rev->usable && !rev->archived)
		BROWSER_ADD(card, axis_web_browser_link("/profiles/assign", "Assign this profile to a library folder"));
	return card;
}

static HtmxResponse *
profile_done(
	AxisWebProfiles *self,
	HtmxRequest     *request,
	PageWork        *work
){
	const AxisProfileRevision *rev = work->current;
	g_autofree gchar *label = axis_web_profiles_label(rev);
	g_autofree gchar *title = g_strdup_printf("Conversion profile: %s", label);
	AxisWebNotice notice;
	g_autoptr(HtmxDiv) content = page_start(request, title, NULL, &notice);
	g_autoptr(GString) where = g_string_new(NULL);
	HtmxNode *card;

	if (!rev->usable)
		BROWSER_ADD(content, axis_web_browser_warning(rev->unusable));
	if (rev->archived)
		BROWSER_ADD(content, axis_web_browser_warning("This conversion profile is archived: it cannot be "
			"assigned. Select Offer this profile again below to use it again."));

	card = axis_web_storage_card("State of this profile");
	{
		HtmxNode *facts = axis_web_browser_facts_new();
		HtmxSpan *lock = htmx_span_new();
		g_autofree gchar *revision = g_strdup_printf("%d", rev->number);

		axis_web_browser_fact(facts, "Kind", kind_label(rev->kind));
		axis_web_browser_fact(facts, "Origin", rev->built_in ? "built in (read only)" : "an administrator's copy");
		axis_web_browser_fact(facts, "Current version", revision);
		BROWSER_ADD(lock, axis_web_profiles_lock_badge(rev));
		axis_web_browser_fact_node(facts, "Locked or proven", HTMX_NODE(lock));
		axis_web_browser_fact(facts, "Assigned to", assigned_roots(work->assignments, rev->slug, where));
		BROWSER_ADD(card, facts);
		BROWSER_ADD(card, axis_web_storage_text(NULL, axis_web_profiles_lock_sentence(rev)));
		if (rev->note[0] != '\0')
			BROWSER_ADD(card, axis_web_storage_text("help", rev->note));
	}
	BROWSER_ADD(content, card);
	BROWSER_ADD(content, definition_card(rev));
	BROWSER_ADD(content, savings_card(request, rev, work->snapshot));
	if (work->compat != NULL)
		BROWSER_ADD(content, axis_web_compat_target_card(work->compat, rev->slug, axis_web_request_is_admin(request)));
	BROWSER_ADD(content, history_card(work->history));
	if (axis_web_request_is_admin(request))
		BROWSER_ADD(content, actions_card(request, rev, &notice));
	axis_web_notice_end(&notice);
	return axis_web_page_respond(request, title, HTMX_NODE(content));
}

/* ── Assignment ─────────────────────────────────────────────────── */

static const AxisProfileAssignment *
assignment_of(
	GPtrArray       *assignments,
	gint64           root,
	AxisProfileKind  kind
){
	guint i;

	for (i = 0; assignments != NULL && i < assignments->len; i++) {
		const AxisProfileAssignment *a = g_ptr_array_index(assignments, i);

		if (a->root_id == root && a->kind == kind)
			return a;
	}
	return NULL;
}

/* Whether a profile is for what a root holds (and may be assigned). */
static gboolean
fits(
	const AxisProfileInfo *profile,
	const gchar           *media_type
){
	AxisMediaType type;

	return profile->current != NULL && profile->current->usable && !profile->archived &&
		axis_media_type_from_string(media_type, &type) &&
		(profile->current->def.libraries & AXIS_TARGET_ROOT(type)) != 0;
}

static const AxisProfileInfo *
find_profile(
	GPtrArray   *profiles,
	const gchar *slug
){
	guint i;

	for (i = 0; profiles != NULL && slug != NULL && i < profiles->len; i++) {
		const AxisProfileInfo *p = g_ptr_array_index(profiles, i);

		if (strcmp(p->slug, slug) == 0)
			return p;
	}
	return NULL;
}

static const Root *
find_root(
	GPtrArray *roots,
	gint64     id
){
	guint i;

	for (i = 0; roots != NULL && i < roots->len; i++) {
		const Root *r = g_ptr_array_index(roots, i);

		if (r->id == id)
			return r;
	}
	return NULL;
}

static HtmxNode *
verdict_badge(AxisCompatVerdict verdict)
{
	return axis_web_browser_badge(axis_compat_verdict_label(verdict), verdict == AXIS_VERDICT_SAFE ? "badge-succeeded" :
		verdict == AXIS_VERDICT_RISKY ? "badge-failed" : "badge-warn");
}

static const gchar *
verdict_sentence(AxisCompatVerdict verdict)
{
	switch (verdict) {
	case AXIS_VERDICT_SAFE:
		return "Every device that plays this library has been seen playing what this profile makes, directly.";
	case AXIS_VERDICT_RISKY:
		return "A device that plays this library is known to fail what this profile makes: the media server had to "
			"convert it. Converted files could stop playing there, and the originals would be gone.";
	default:
		return "It is not known whether every device that plays this library can play what this profile makes "
			"(a device never seen with it, or no devices, or a library the media server does not map). Unknown is "
			"never safe.";
	}
}

/* The saving of a profile on a root, from the snapshot's per-root figure. */
static const AxisEstimateFact *
root_fact(
	const AxisEstimateSnapshot *snapshot,
	const gchar                *slug,
	gint64                      root
){
	g_autofree gchar *dim = g_strdup_printf("%" G_GINT64_FORMAT, root);

	return axis_estimate_snapshot_fact(snapshot, slug, "root", dim);
}

/* The pairing asked about, with the form that assigns it.  @insist: the
 * confirmation was missing (a POST came without it). */
static HtmxNode *
pairing_card(
	HtmxRequest                *request,
	const AxisEstimateSnapshot *snapshot,
	AxisCompatReport           *compat,
	const Root                 *root,
	const AxisProfileInfo      *profile,
	gboolean                    insist
){
	g_autofree gchar *label = axis_web_profiles_label(profile->current);
	g_autofree gchar *heading = g_strdup_printf("%s on %s", label, root->name);
	HtmxNode *card = axis_web_storage_card(heading);
	const AxisEstimateFact *fact = root_fact(snapshot, profile->slug, root->id);
	AxisCompatVerdict verdict = compat != NULL ? axis_compat_report_root_verdict(compat, root->id, profile->slug, NULL) :
		AXIS_VERDICT_UNKNOWN;
	HtmxNode *facts = axis_web_browser_facts_new();
	HtmxSpan *badge = htmx_span_new();

	if (fact != NULL) {
		g_autofree gchar *saving = saving_text(fact);
		g_autofree gchar *text = g_strdup_printf("%s, from %" G_GINT64_FORMAT " file%s (estimates of version %d)",
			saving, fact->paths, fact->paths == 1 ? "" : "s", fact->revision);

		axis_web_browser_fact(facts, "Estimated saving", text);
	} else {
		axis_web_browser_fact(facts, "Estimated saving", "not estimated yet: assigned profiles are estimated at the "
			"next refresh");
	}
	BROWSER_ADD(badge, verdict_badge(verdict));
	axis_web_browser_fact_node(facts, "Playback verdict", HTMX_NODE(badge));
	BROWSER_ADD(card, facts);
	BROWSER_ADD(card, axis_web_storage_text("help", compat != NULL ? verdict_sentence(verdict) :
		"Player compatibility is not set up, so the playback verdict is unknown. Connect a media server in the "
		"Setup guide, step Players."));
	if (insist)
		BROWSER_ADD(card, axis_web_auth_notice(TRUE, "This verdict is not safe: confirm it to assign the profile."));
	if (axis_web_request_is_admin(request)) {
		HtmxNode *form = axis_web_auth_post_form(request, "/profiles/assign");
		g_autofree gchar *root_id = g_strdup_printf("%" G_GINT64_FORMAT, root->id);

		htmx_element_add_class(HTMX_ELEMENT(form), "form-wide");
		axis_web_auth_add(form, axis_web_auth_hidden("root", root_id));
		axis_web_auth_add(form, axis_web_auth_hidden("profile", profile->slug));
		if (verdict != AXIS_VERDICT_SAFE) {
			g_autofree gchar *text = g_strdup_printf("The verdict for this pairing is %s. I have read it and assign "
				"the profile anyway (it converts nothing now; the verdict is checked again before any conversion)",
				verdict == AXIS_VERDICT_RISKY ? "risky" : "unknown");

			axis_web_auth_add(form, axis_web_storage_checkbox("assign-confirm", text, "confirm", TRUE));
		}
		axis_web_auth_add(form, axis_web_auth_button("Assign this profile to this library folder", "button-primary"));
		BROWSER_ADD(card, form);
	}
	return card;
}

static HtmxResponse *
assign_page(
	HtmxRequest *request,
	PageWork    *work,
	gboolean     insist,
	guint        status
){
	static const AxisWebColumn columns[] = {
		{ "Library folder", AXIS_WEB_COL_NAME },
		{ "Holds", AXIS_WEB_COL_TEXT },
		{ "Video profile", AXIS_WEB_COL_WIDE },
		{ "Audio profile", AXIS_WEB_COL_WIDE },
		{ "Assign a profile", AXIS_WEB_COL_CONTROLS },
		{ NULL, 0 }
	};
	AxisWebNotice notice;
	g_autoptr(HtmxDiv) content = page_start(request, "Assign conversion profiles to library folders", NULL, &notice);
	HtmxNode *card = axis_web_storage_card("Library folders");
	HtmxNode *table = axis_web_table_new("assignments", columns);
	HtmxTbody *body = htmx_tbody_new();
	gboolean admin = axis_web_request_is_admin(request);
	HtmxResponse *response;
	guint i;

	BROWSER_ADD(content, axis_web_storage_text(NULL, "Each library folder has at most one video and one audio "
		"conversion profile. Assigning one records what the folder should be converted with: it converts nothing, "
		"changes no file and starts no job. The Space savings and Player compatibility pages show the assigned "
		"profile's figures for the folder, and a conversion starts from it."));
	if (work->pairing != NULL) {
		const AxisProfileInfo *profile = find_profile(work->profiles, work->pairing);
		const Root *root = find_root(work->roots, work->root);

		if (profile != NULL && root != NULL && fits(profile, root->media_type))
			BROWSER_ADD(content, pairing_card(request, work->snapshot, work->compat, root, profile, insist));
		else
			BROWSER_ADD(content, axis_web_auth_notice(TRUE, "That profile cannot be assigned to that library "
				"folder."));
	}
	for (i = 0; work->roots != NULL && i < work->roots->len; i++) {
		const Root *root = g_ptr_array_index(work->roots, i);
		AxisMediaType type = AXIS_MEDIA_OTHER;
		HtmxTr *row = htmx_tr_new();
		HtmxTd *choose = htmx_td_new();
		gboolean any = FALSE;
		guint k;

		axis_media_type_from_string(root->media_type, &type);
		{
			g_autofree gchar *row_id = g_strdup_printf("assign-row-%" G_GINT64_FORMAT, root->id);
			HtmxTd *first = htmx_td_new();
			HtmxSpan *name = htmx_span_new();

			htmx_element_set_id(HTMX_ELEMENT(row), row_id);
			axis_web_notice_put(&notice, HTMX_NODE(first), row_id);
			htmx_node_set_text_content(HTMX_NODE(name), root->name);
			BROWSER_ADD(first, name);
			BROWSER_ADD(row, first);
		}
		BROWSER_ADD(row, text_cell(axis_media_type_label(type)));
		for (k = 0; k < 2; k++) {
			AxisProfileKind kind = k == 0 ? AXIS_PROFILE_KIND_VIDEO : AXIS_PROFILE_KIND_AUDIO;
			const AxisProfileAssignment *a = assignment_of(work->assignments, root->id, kind);
			const AxisProfileInfo *p = a != NULL ? find_profile(work->profiles, a->slug) : NULL;
			HtmxTd *cell = htmx_td_new();

			if (a == NULL) {
				htmx_node_set_text_content(HTMX_NODE(cell), "none");
			} else {
				g_autofree gchar *href = profile_href(a->slug);
				g_autofree gchar *name = p != NULL && p->current != NULL ? axis_web_profiles_label(p->current) :
					g_strdup(a->slug);
				g_autofree gchar *verdict_word = g_strdup(a->verdict);
				g_autofree gchar *note = NULL;
				const AxisEstimateFact *fact = root_fact(work->snapshot, a->slug, root->id);

				if (verdict_word != NULL && verdict_word[0] != '\0')
					verdict_word[0] = g_ascii_toupper(verdict_word[0]);
				note = g_strdup_printf("assigned by %s; playback verdict then: %s%s", a->assigned_by,
					verdict_word != NULL ? verdict_word : "Unknown", a->confirmed ? " (confirmed)" : "");
				BROWSER_ADD(cell, axis_web_browser_link(href, name));
				BROWSER_ADD(cell, axis_web_storage_text("help", note));
				if (fact != NULL) {
					g_autofree gchar *saving = saving_text(fact);
					g_autofree gchar *text = g_strdup_printf("saves %s", saving);

					BROWSER_ADD(cell, axis_web_storage_text("help", text));
				}
				if (work->compat != NULL) {
					AxisCompatVerdict verdict = axis_compat_report_root_verdict(work->compat, root->id, a->slug, NULL);

					BROWSER_ADD(cell, verdict_badge(verdict));
				}
				if (admin) {
					HtmxNode *form = axis_web_auth_post_form(request, "/profiles/unassign");
					g_autofree gchar *root_id = g_strdup_printf("%" G_GINT64_FORMAT, root->id);

					htmx_element_add_class(HTMX_ELEMENT(form), "inline-form");
					axis_web_auth_add(form, axis_web_auth_hidden("root", root_id));
					axis_web_auth_add(form, axis_web_auth_hidden("kind", axis_profile_kind_to_string(kind)));
					axis_web_auth_add(form, axis_web_auth_button("Unassign this profile", "button-quiet"));
					BROWSER_ADD(cell, form);
				}
			}
			BROWSER_ADD(row, cell);
		}
		/* The choice: a GET to this page, which shows the pairing. */
		{
			HtmxForm *form = htmx_form_new();
			HtmxSelect *select = htmx_select_new_with_name("profile");
			g_autofree gchar *root_id = g_strdup_printf("%" G_GINT64_FORMAT, root->id);
			g_autofree gchar *select_id = g_strdup_printf("assign-%" G_GINT64_FORMAT, root->id);
			HtmxLabel *label = htmx_label_new_for(select_id);
			HtmxInput *hidden = htmx_input_new(HTMX_INPUT_HIDDEN);

			htmx_element_set_attribute(HTMX_ELEMENT(form), "method", "get");
			htmx_element_set_attribute(HTMX_ELEMENT(form), "action", "/profiles/assign");
			htmx_element_add_class(HTMX_ELEMENT(form), "inline-form");
			htmx_input_set_name(hidden, "root");
			htmx_input_set_value(hidden, root_id);
			htmx_element_set_id(HTMX_ELEMENT(select), select_id);
			htmx_node_set_text_content(HTMX_NODE(label), "Profile");
			for (k = 0; work->profiles != NULL && k < work->profiles->len; k++) {
				const AxisProfileInfo *p = g_ptr_array_index(work->profiles, k);
				g_autofree gchar *name = NULL;

				if (!fits(p, root->media_type))
					continue;
				name = axis_web_profiles_label(p->current);
				BROWSER_ADD(select, htmx_option_new_with_value(p->slug, name));
				any = TRUE;
			}
			if (any) {
				BROWSER_ADD(form, hidden);
				BROWSER_ADD(form, label);
				BROWSER_ADD(form, select);
				BROWSER_ADD(form, axis_web_auth_button("Show the saving and playback verdict", NULL));
				BROWSER_ADD(choose, form);
			} else {
				g_object_unref(form);
				g_object_unref(select);
				g_object_unref(label);
				g_object_unref(hidden);
				htmx_node_set_text_content(HTMX_NODE(choose), "No conversion profile is for what this library folder "
					"holds. Copy one under Conversion profiles and tick this kind of library on its edit page.");
			}
		}
		BROWSER_ADD(row, choose);
		BROWSER_ADD(body, row);
	}
	BROWSER_ADD(table, body);
	if (work->roots == NULL || work->roots->len == 0) {
		g_object_unref(table);
		BROWSER_ADD(card, axis_web_storage_text(NULL, "There is no library folder yet. Add one under Media "
			"management, Storage."));
	} else {
		BROWSER_ADD(card, table);
	}
	BROWSER_ADD(content, card);
	axis_web_notice_end(&notice);
	response = axis_web_page_respond(request, "Assign conversion profiles to library folders", HTMX_NODE(content));
	if (status != 200)
		htmx_response_set_status(response, status);
	return response;
}

/* ── Handlers ───────────────────────────────────────────────────── */

static HtmxResponse *
page_done(
	AxisWebProfiles *self,
	HtmxRequest     *request,
	gpointer         data
){
	PageWork *work = data;

	if (work->error != NULL)
		return axis_web_profiles_error(request, work->error);
	switch (work->page) {
	case PAGE_PROFILE:
		if (work->current == NULL)
			return not_found(request);
		return profile_done(self, request, work);
	case PAGE_ASSIGN:
		return assign_page(request, work, FALSE, 200);
	default:
		return list_done(self, request, work);
	}
}

static HtmxResponse *
handle_list(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	static const gchar *const names[] = { "archived", NULL };
	const gchar *archived = htmx_request_get_query_param(request, "archived");
	PageWork *work;

	if (!query_ok(request, "unassigned", names) || (archived != NULL && strcmp(archived, "yes") != 0))
		return bad_request(request);
	work = g_new0(PageWork, 1);
	work->page = PAGE_LIST;
	work->archived = archived != NULL;
	return axis_web_profiles_dispatch(user_data, request, page_run, page_done, work, page_work_free);
}

static HtmxResponse *
handle_profile(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	const gchar *slug = params != NULL ? g_hash_table_lookup(params, "slug") : NULL;
	PageWork *work;

	if (!axis_profile_slug_valid(slug))
		return not_found(request);
	if (!query_ok(request, "copied saved unchanged archived restored", NULL))
		return bad_request(request);
	work = g_new0(PageWork, 1);
	work->page = PAGE_PROFILE;
	work->slug = g_strdup(slug);
	return axis_web_profiles_dispatch(user_data, request, page_run, page_done, work, page_work_free);
}

static HtmxResponse *
handle_assign_page(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	static const gchar *const names[] = { "root", "profile", NULL };
	const gchar *root = htmx_request_get_query_param(request, "root");
	const gchar *profile = htmx_request_get_query_param(request, "profile");
	PageWork *work;
	guint64 id = 0;

	if (!query_ok(request, "assigned unassigned", names) || (root == NULL) != (profile == NULL))
		return bad_request(request);
	if (root != NULL && (!g_ascii_string_to_unsigned(root, 10, 1, G_MAXINT64, &id, NULL) ||
	                     !axis_profile_slug_valid(profile)))
		return bad_request(request);
	work = g_new0(PageWork, 1);
	work->page = PAGE_ASSIGN;
	work->root = (gint64)id;
	work->pairing = g_strdup(profile);
	return axis_web_profiles_dispatch(user_data, request, page_run, page_done, work, page_work_free);
}

/* ── Actions ────────────────────────────────────────────────────── */

typedef enum
{
	ACTION_COPY,
	ACTION_ARCHIVE,
	ACTION_RESTORE,
	ACTION_ASSIGN,
	ACTION_UNASSIGN
} ActionId;

typedef struct
{
	ActionId          action;
	gchar            *slug;
	gchar            *name;
	gchar            *user;
	gchar            *created;
	gint64            root;
	AxisProfileKind   kind;
	gboolean          confirmed;
	gboolean          changed;
	gchar            *verdict;
	gboolean          insist;
	PageWork         *page;	/* the assignment page again, for a missing confirmation */
	GError           *error;
} ActionWork;

static void
action_work_free(gpointer data)
{
	ActionWork *work = data;

	g_free(work->slug);
	g_free(work->name);
	g_free(work->user);
	g_free(work->created);
	g_free(work->verdict);
	if (work->page != NULL)
		page_work_free(work->page);
	g_clear_error(&work->error);
	g_free(work);
}

static void
action_run(
	AxisWebProfiles *self,
	gpointer         data
){
	ActionWork *work = data;
	AxisDbPool *pool = axis_estimator_get_pool(self->estimator);

	switch (work->action) {
	case ACTION_COPY: {
		/* The default name is the original's, "Copy of" before it. */
		if (work->name == NULL) {
			g_autoptr(AxisProfileRevision) source = NULL;
			AxisDbConn *conn = axis_db_pool_acquire(pool, &work->error);
			gboolean ok;

			if (conn == NULL)
				return;
			ok = axis_profile_store_current(conn, work->slug, &source, &work->error);
			axis_db_pool_release(pool, conn);
			if (!ok)
				return;
			if (source == NULL) {
				g_set_error_literal(&work->error, AXIS_PROFILE_ERROR, AXIS_PROFILE_ERROR_NOT_FOUND,
					"There is no such profile.");
				return;
			}
			{
				g_autofree gchar *raw = g_strdup_printf("Copy of %s", source->name);

				work->name = axis_profile_clean_text(raw, AXIS_PROFILE_NAME_MAX_CHARS, FALSE);
			}
		}
		axis_profile_store_copy(pool, work->slug, work->name, work->user, &work->created, &work->error);
		break;
	}
	case ACTION_ARCHIVE:
	case ACTION_RESTORE:
		axis_profile_store_set_archived(pool, work->slug, work->action == ACTION_ARCHIVE, &work->error);
		break;
	case ACTION_ASSIGN: {
		/* The verdict now, worked out here: never what a form says. */
		g_autoptr(AxisCompatReport) report = axis_web_profiles_compat_report(self);
		AxisCompatVerdict verdict = report != NULL ?
			axis_compat_report_root_verdict(report, work->root, work->slug, NULL) : AXIS_VERDICT_UNKNOWN;

		work->verdict = g_strdup(axis_compat_verdict_to_string(verdict));
		if (verdict != AXIS_VERDICT_SAFE && !work->confirmed) {
			/* The page again, with the confirmation it needs. */
			work->insist = TRUE;
			work->page = g_new0(PageWork, 1);
			work->page->page = PAGE_ASSIGN;
			work->page->root = work->root;
			work->page->pairing = g_strdup(work->slug);
			page_run(self, work->page);
			return;
		}
		axis_profile_store_assign(pool, work->root, work->slug, work->verdict, work->confirmed, work->user,
			&work->error);
		break;
	}
	case ACTION_UNASSIGN:
		axis_profile_store_unassign(pool, work->root, work->kind, &work->changed, &work->error);
		break;
	}
}

static HtmxResponse *
action_done(
	AxisWebProfiles *self,
	HtmxRequest     *request,
	gpointer         data
){
	ActionWork *work = data;
	g_autofree gchar *location = NULL;

	if (work->insist) {
		if (work->page->error != NULL)
			return axis_web_profiles_error(request, work->page->error);
		return assign_page(request, work->page, TRUE, 409);
	}
	if (work->error != NULL)
		return axis_web_profiles_error(request, work->error);
	switch (work->action) {
	case ACTION_COPY:
		axis_web_storage_audit(request, "profile %s copied to %s", work->slug, work->created);
		location = g_strdup_printf("/profiles/%s?done=copied", work->created);
		return axis_web_auth_redirect(request, location);
	case ACTION_ARCHIVE:
	case ACTION_RESTORE:
		axis_web_storage_audit(request, "profile %s %s", work->slug, work->action == ACTION_ARCHIVE ? "archived" :
			"restored");
		location = g_strdup_printf("/profiles/%s", work->slug);
		return axis_web_place_redirect(request, location, work->action == ACTION_ARCHIVE ? "archived" : "restored",
			"change-it");
	case ACTION_ASSIGN:
		axis_web_storage_audit(request, "profile %s assigned to root %" G_GINT64_FORMAT " (verdict %s%s)", work->slug,
			work->root, work->verdict, work->confirmed ? ", confirmed" : "");
		location = g_strdup_printf("assign-row-%" G_GINT64_FORMAT, work->root);
		return axis_web_place_redirect(request, "/profiles/assign", "assigned", location);
	default:
		axis_web_storage_audit(request, "%s profile of root %" G_GINT64_FORMAT " unassigned: %s",
			axis_profile_kind_to_string(work->kind), work->root, work->changed ? "yes" : "there was none");
		location = g_strdup_printf("assign-row-%" G_GINT64_FORMAT, work->root);
		return axis_web_place_redirect(request, "/profiles/assign", "unassigned", location);
	}
}

static HtmxResponse *
start_action(
	HtmxRequest *request,
	gpointer     user_data,
	ActionWork  *work
){
	work->user = g_strdup(axis_web_request_get_username(request));
	return axis_web_profiles_dispatch(user_data, request, action_run, action_done, work, action_work_free);
}

static HtmxResponse *
handle_copy(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	const gchar *slug = params != NULL ? g_hash_table_lookup(params, "slug") : NULL;
	const gchar *name = htmx_request_get_form_value(request, "name");
	ActionWork *work;

	if (!axis_profile_slug_valid(slug))
		return not_found(request);
	work = g_new0(ActionWork, 1);
	work->action = ACTION_COPY;
	work->slug = g_strdup(slug);
	/* A name is a person's text: cleaned and capped, else the default. */
	if (name != NULL && name[0] != '\0') {
		work->name = axis_profile_clean_text(name, AXIS_PROFILE_NAME_MAX_CHARS, FALSE);
		if (work->name == NULL) {
			action_work_free(work);
			return axis_web_error_respond(request, 400, "Bad request", "The name is not text that can be shown.");
		}
	}
	return start_action(request, user_data, work);
}

static HtmxResponse *
handle_archive(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data,
	gboolean     archive
){
	const gchar *slug = params != NULL ? g_hash_table_lookup(params, "slug") : NULL;
	ActionWork *work;

	if (!axis_profile_slug_valid(slug))
		return not_found(request);
	work = g_new0(ActionWork, 1);
	work->action = archive ? ACTION_ARCHIVE : ACTION_RESTORE;
	work->slug = g_strdup(slug);
	return start_action(request, user_data, work);
}

static HtmxResponse *
handle_archive_post(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	return handle_archive(request, params, user_data, TRUE);
}

static HtmxResponse *
handle_restore_post(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	return handle_archive(request, params, user_data, FALSE);
}

static HtmxResponse *
handle_assign(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	const gchar *profile = htmx_request_get_form_value(request, "profile");
	guint64 root = 0;
	ActionWork *work;

	if (!axis_web_storage_form_u64(request, "root", &root) || root == 0 || root > G_MAXINT64 ||
	    !axis_profile_slug_valid(profile))
		return axis_web_error_respond(request, 400, "Bad request", "Choose a library folder and a profile.");
	work = g_new0(ActionWork, 1);
	work->action = ACTION_ASSIGN;
	work->root = (gint64)root;
	work->slug = g_strdup(profile);
	work->confirmed = axis_web_storage_form_yes(request, "confirm");
	return start_action(request, user_data, work);
}

static HtmxResponse *
handle_unassign(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	const gchar *kind = htmx_request_get_form_value(request, "kind");
	guint64 root = 0;
	ActionWork *work;
	AxisProfileKind parsed;

	if (!axis_web_storage_form_u64(request, "root", &root) || root == 0 || root > G_MAXINT64 ||
	    !axis_profile_kind_from_string(kind, &parsed))
		return axis_web_error_respond(request, 400, "Bad request", "Choose a library folder and a kind.");
	work = g_new0(ActionWork, 1);
	work->action = ACTION_UNASSIGN;
	work->root = (gint64)root;
	work->kind = parsed;
	return start_action(request, user_data, work);
}

void
axis_web_profiles_register_pages(
	AxisWebProfiles *self,
	AxisWeb         *web
){
	axis_web_get(web, "/profiles", handle_list, self);
	axis_web_route_full(web, HTMX_METHOD_GET, "/profiles/assign", AXIS_WEB_ROUTE_ADMIN, handle_assign_page, self);
	axis_web_get(web, "/profiles/:slug", handle_profile, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/profiles/assign", AXIS_WEB_ROUTE_ADMIN, handle_assign, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/profiles/unassign", AXIS_WEB_ROUTE_ADMIN, handle_unassign, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/profiles/:slug/copy", AXIS_WEB_ROUTE_ADMIN, handle_copy, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/profiles/:slug/archive", AXIS_WEB_ROUTE_ADMIN, handle_archive_post,
		self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/profiles/:slug/restore", AXIS_WEB_ROUTE_ADMIN, handle_restore_post,
		self);
}
