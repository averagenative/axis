/*
 * axis-web-quality-pages.c - the quality profile list, a profile's page,
 * Try a name, copy and delete
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A profile's name and a tried release name are a person's text: text
 * content only (an input's value is an attribute, escaped).  Links carry
 * slugs.  Trying a name reads it and decides it: no request leaves the
 * daemon.
 */

#include "web/axis-web-quality-private.h"

#include <math.h>
#include <string.h>

#include "compat/axis-compat-report.h"

/* ── Pieces ─────────────────────────────────────────────────────── */

static HtmxResponse *
not_found(HtmxRequest *request)
{
	return axis_web_error_respond(request, 404, "Not found", "There is no such quality profile.");
}

static HtmxResponse *
bad_request(HtmxRequest *request)
{
	return axis_web_error_respond(request, 400, "Bad request", "The address is not valid.");
}

static HtmxNode *
text_cell(const gchar *text)
{
	HtmxTd *cell = htmx_td_new();

	htmx_node_set_text_content(HTMX_NODE(cell), text);
	return HTMX_NODE(cell);
}

static const gchar *
done_message(const gchar *done)
{
	static const struct
	{
		const gchar *key;
		const gchar *text;
	} messages[] = {
		{ "copied", "The copy has been made. Edit it to make it your own; a title uses it once it is chosen for one." },
		{ "saved", "The profile has been saved. Searches from now on decide with it." },
		{ "unchanged", "Nothing had changed, so nothing was saved." },
		{ "deleted", "The profile has been deleted." }
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
	HtmxRequest *request,
	const gchar *title,
	const gchar *intro
){
	HtmxDiv *content = htmx_div_new();
	HtmxHeading *heading = htmx_h1_new();
	const gchar *done = done_message(htmx_request_get_query_param(request, "done"));

	htmx_node_set_text_content(HTMX_NODE(heading), title);
	BROWSER_ADD(content, heading);
	if (intro != NULL)
		BROWSER_ADD(content, axis_web_page_intro(intro));
	if (done != NULL)
		BROWSER_ADD(content, axis_web_auth_notice(FALSE, done));
	return content;
}

/* Only "done" (one of @allowed) and the names in @names may be given. */
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
		} else if (names == NULL || !g_strv_contains(names, key)) {
			return FALSE;
		}
	}
	return TRUE;
}

/* The qualities of a profile, best first, tiers joined by "="; a long list
 * by its ends (the profile's page has it whole). */
static gchar *
qualities_text(const AxisQualityProfile *p)
{
	GString *out = g_string_new(NULL);
	guint i;

	if (p->n_allowed > 6) {
		g_string_append_printf(out, "%u qualities, from %s down to %s", p->n_allowed, axis_quality_label(p->allowed[0]),
			axis_quality_label(p->allowed[p->n_allowed - 1]));
		if (p->allow_unknown)
			g_string_append(out, ", then unknown");
		return g_string_free(out, FALSE);
	}
	for (i = 0; i < p->n_allowed && i < AXIS_QUALITY_N_LADDER; i++) {
		if (i > 0)
			g_string_append(out, p->tier[i] == p->tier[i - 1] ? " = " : ", ");
		g_string_append(out, axis_quality_label(p->allowed[i]));
	}
	if (p->allow_unknown)
		g_string_append(out, ", unknown");
	return g_string_free(out, FALSE);
}

static const gchar *
origin_text(
	const AxisQualityStored *s,
	gchar                  **owned
){
	*owned = NULL;
	if (s->built_in)
		return "built in";
	if (s->copied_from != NULL)
		return *owned = g_strdup_printf("a copy of %s", s->copied_from);
	return "a copy";
}

/* ── Work ───────────────────────────────────────────────────────── */

typedef enum
{
	PAGE_LIST,
	PAGE_PROFILE,
	PAGE_TRY
} PageId;

typedef struct
{
	PageId              page;
	gchar              *slug;
	GPtrArray          *profiles;	/* AxisQualityStored */
	GPtrArray          *uses;	/* per profile of @profiles: GPtrArray of utf8 */
	AxisQualityStored  *stored;	/* the profile of the page */
	GPtrArray          *stored_uses;
	gchar              *default_slug;
	/* Trying a name. */
	gchar              *name;
	gchar              *runtime_text;
	gchar              *size_text;
	gint                runtime;
	gint64              size;
	gchar              *input_error;
	AxisRelease        *release;
	AxisQualityVerdict *verdict;
	guint               n_devices;	/* players considered */
	gboolean            players_read;
	GError             *error;
} PageWork;

static void
page_work_free(gpointer data)
{
	PageWork *work = data;

	g_free(work->slug);
	g_clear_pointer(&work->profiles, g_ptr_array_unref);
	g_clear_pointer(&work->uses, g_ptr_array_unref);
	axis_quality_stored_free(work->stored);
	g_clear_pointer(&work->stored_uses, g_ptr_array_unref);
	g_free(work->default_slug);
	g_free(work->name);
	g_free(work->runtime_text);
	g_free(work->size_text);
	g_free(work->input_error);
	axis_release_free(work->release);
	axis_quality_verdict_free(work->verdict);
	g_clear_error(&work->error);
	g_free(work);
}

/* The household's players as their evidence: every device seen playing in
 * the look-back window, or pinned.  @infos keeps what the observations
 * point into. */
static void
read_players(
	AxisWebQuality *self,
	GPtrArray      *infos,
	GArray         *devices,
	GPtrArray      *observations
){
	g_autoptr(GPtrArray) list = NULL;
	g_autoptr(GError) error = NULL;
	guint i;
	guint k;

	if (self->compat == NULL || !axis_compat_is_enabled(self->compat))
		return;
	if (!axis_compat_list_devices(self->compat, &list, &error)) {
		g_message("quality: the household's players could not be read: %s", error->message);
		return;
	}
	for (i = 0; i < list->len && i < 64; i++) {
		const AxisCompatDeviceInfo *d = g_ptr_array_index(list, i);
		g_autofree gchar *handle = NULL;
		AxisCompatDeviceInfo *info = NULL;
		GArray *obs;
		AxisQualityDevice device;

		if (d->plays_in_window == 0 && !d->pinned)
			continue;
		handle = axis_compat_handle(d->server_kind, d->client_id);
		if (handle == NULL || !axis_compat_get_device(self->compat, handle, &info, NULL) || info == NULL)
			continue;
		g_ptr_array_add(infos, info);
		obs = g_array_new(FALSE, TRUE, sizeof(AxisCompatObservation));
		for (k = 0; info->rows != NULL && k < info->rows->len; k++) {
			const AxisCompatObsRow *row = g_ptr_array_index(info->rows, k);

			g_array_append_val(obs, row->obs);
		}
		g_ptr_array_add(observations, obs);
		device.obs = (const AxisCompatObservation *)obs->data;
		device.n_obs = obs->len;
		g_array_append_val(devices, device);
	}
}

static void
decide_try(
	AxisWebQuality *self,
	PageWork       *work
){
	g_autoptr(GPtrArray) infos = g_ptr_array_new_with_free_func((GDestroyNotify)axis_compat_device_info_free);
	g_autoptr(GPtrArray) observations = g_ptr_array_new_with_free_func((GDestroyNotify)g_array_unref);
	g_autoptr(GArray) devices = g_array_new(FALSE, TRUE, sizeof(AxisQualityDevice));
	AxisQualityPlayers players;
	AxisQualityContext ctx;
	AxisQualityTitle title;
	AxisQualityCandidate candidate;

	work->release = axis_release_parse(work->name);
	memset(&ctx, 0, sizeof(ctx));
	memset(&title, 0, sizeof(title));
	memset(&candidate, 0, sizeof(candidate));
	title.noun = axis_release_is_episode(work->release) ? "episode" : "movie";
	title.runtime_minutes = work->runtime;
	ctx.profile = &work->stored->profile;
	ctx.title = &title;
	ctx.now_us = g_get_real_time();
	ctx.no_title = TRUE;
	if (work->stored->profile.players > 0) {
		read_players(self, infos, devices, observations);
		work->players_read = TRUE;
		work->n_devices = devices->len;
		players.devices = (const AxisQualityDevice *)devices->data;
		players.n_devices = devices->len;
		if (devices->len > 0) {
			ctx.players = axis_quality_players_verdict;
			ctx.players_data = &players;
		}
	}
	candidate.release = work->release;
	candidate.name = work->name;
	candidate.match = AXIS_RELEASE_MATCH_NONE;
	candidate.size_bytes = work->size;
	candidate.protocol = AXIS_DOWNLOAD_USENET;
	candidate.seeders = -1;
	work->verdict = axis_quality_decide_one(&ctx, &candidate);
}

static void
page_run(
	AxisWebQuality *self,
	gpointer        data
){
	PageWork *work = data;
	guint i;

	work->default_slug = axis_quality_store_default_slug(self->store);
	if (!axis_quality_store_list(self->store, &work->profiles, &work->error))
		return;
	if (work->page == PAGE_LIST) {
		work->uses = g_ptr_array_new_with_free_func((GDestroyNotify)g_ptr_array_unref);
		for (i = 0; i < work->profiles->len; i++) {
			const AxisQualityStored *s = g_ptr_array_index(work->profiles, i);
			GPtrArray *uses = NULL;

			if (!axis_quality_store_uses(self->store, s->slug, &uses, &work->error))
				return;
			g_ptr_array_add(work->uses, uses);
		}
		return;
	}
	for (i = 0; i < work->profiles->len; i++) {
		const AxisQualityStored *s = g_ptr_array_index(work->profiles, i);

		if (g_strcmp0(s->slug, work->slug) == 0 && !axis_quality_store_get(self->store, s->slug, &work->stored,
		        &work->error))
			return;
	}
	if (work->stored == NULL)
		return;
	if (work->page == PAGE_PROFILE) {
		axis_quality_store_uses(self->store, work->slug, &work->stored_uses, &work->error);
		return;
	}
	if (work->name != NULL && work->input_error == NULL && work->stored->usable)
		decide_try(self, work);
}

/* ── The list ───────────────────────────────────────────────────── */

static HtmxNode *
fixed_rules_text(void)
{
	return axis_web_storage_text("help", "Whatever a profile says, these are never taken: a sample, a "
		"password-protected release, a name that ends in an archive's or a program's extension, a name with no "
		"readable title, and a release that is not the title asked for or could be more than one. Cinema "
		"recordings and pre-release copies are taken only by a profile whose own switch for them is on.");
}

static HtmxResponse *
list_done(
	AxisWebQuality *self,
	HtmxRequest    *request,
	PageWork       *work
){
	static const AxisWebColumn columns[] = {
		{ "Quality profile", AXIS_WEB_COL_NAME },
		{ "Qualities, best first", AXIS_WEB_COL_WIDE },
		{ "Upgraded until", AXIS_WEB_COL_TEXT },
		{ "Used by", AXIS_WEB_COL_WIDE },
		{ "Origin", AXIS_WEB_COL_TEXT },
		{ NULL, 0 }
	};
	g_autoptr(HtmxDiv) content = page_start(request, "Quality profiles", "Which releases of a movie or a series are "
		"acceptable to download, which of them is best, and when one is worth fetching over the file already on "
		"disk; every title follows one profile. This is about downloading: re-encoding files you already have is "
		"Conversion profiles.");
	HtmxNode *card = axis_web_storage_card("All quality profiles");
	HtmxNode *table = axis_web_table_new("quality-profiles", columns);
	HtmxTbody *body = htmx_tbody_new();
	HtmxNode *try_card = axis_web_storage_card("Try a release name");
	guint i;

	BROWSER_ADD(content, axis_web_storage_text(NULL, "Each decision states its reasons. The built-in profiles "
		"cannot be changed; copy one to make your own."));
	BROWSER_ADD(content, fixed_rules_text());
	for (i = 0; i < work->profiles->len; i++) {
		const AxisQualityStored *s = g_ptr_array_index(work->profiles, i);
		GPtrArray *uses = work->uses != NULL && i < work->uses->len ? g_ptr_array_index(work->uses, i) : NULL;
		g_autofree gchar *href = axis_web_quality_href(s->slug);
		g_autofree gchar *origin_owned = NULL;
		const gchar *origin = origin_text(s, &origin_owned);
		HtmxTr *row = htmx_tr_new();
		HtmxTd *name = htmx_td_new();

		BROWSER_ADD(name, axis_web_browser_link(href, s->name));
		if (g_strcmp0(s->slug, work->default_slug) == 0)
			BROWSER_ADD(name, axis_web_browser_badge("Default", "badge-note"));
		if (!s->usable)
			BROWSER_ADD(name, axis_web_browser_badge("Not usable", "badge-failed"));
		BROWSER_ADD(row, name);
		if (s->usable) {
			g_autofree gchar *qualities = qualities_text(&s->profile);

			BROWSER_ADD(row, text_cell(qualities));
			BROWSER_ADD(row, text_cell(s->profile.upgrades ? axis_quality_label(s->profile.cutoff) : "no upgrades"));
		} else {
			BROWSER_ADD(row, text_cell(s->unusable));
			BROWSER_ADD(row, text_cell(""));
		}
		if (uses != NULL && uses->len > 0) {
			g_autofree gchar *what = NULL;
			GString *joined = g_string_new(NULL);
			guint k;

			for (k = 0; k < uses->len; k++)
				g_string_append_printf(joined, "%s%s", k > 0 ? "; " : "", (const gchar *)g_ptr_array_index(uses, k));
			what = g_string_free(joined, FALSE);
			BROWSER_ADD(row, text_cell(what));
		} else {
			BROWSER_ADD(row, text_cell("nothing yet"));
		}
		BROWSER_ADD(row, text_cell(origin));
		BROWSER_ADD(body, row);
	}
	BROWSER_ADD(table, body);
	BROWSER_ADD(card, table);
	BROWSER_ADD(card, axis_web_storage_text("help", "\"=\" joins qualities that rank as equals. What uses a "
		"quality profile: the default for new movies and series (Settings, Quality profiles), and each movie and "
		"series set to it."));
	BROWSER_ADD(content, card);

	BROWSER_ADD(try_card, axis_web_quality_try_form(work->profiles, NULL, work->default_slug, NULL, NULL));
	BROWSER_ADD(content, try_card);
	(void)self;
	return axis_web_page_respond(request, "Quality profiles", HTMX_NODE(content));
}

/* ── A profile ──────────────────────────────────────────────────── */

static HtmxNode *
sizes_card(const AxisQualityProfile *p)
{
	static const AxisWebColumn columns[] = {
		{ "Quality, best first", AXIS_WEB_COL_NAME },
		{ "MiB per minute", AXIS_WEB_COL_TEXT },
		{ "For a 2-hour movie", AXIS_WEB_COL_RANGE },
		{ NULL, 0 }
	};
	HtmxNode *card = axis_web_storage_card("Qualities and sizes");
	HtmxNode *table = axis_web_table_new("quality-sizes", columns);
	HtmxTbody *body = htmx_tbody_new();
	guint i;

	for (i = 0; i < p->n_allowed && i < AXIS_QUALITY_N_LADDER; i++) {
		const AxisQualitySize *s = &p->sizes[p->allowed[i]];
		g_autofree gchar *name = i > 0 && p->tier[i] == p->tier[i - 1] ?
			g_strdup_printf("= %s", axis_quality_label(p->allowed[i])) :
			g_strdup_printf("%u. %s", p->tier[i] + 1, axis_quality_label(p->allowed[i]));
		g_autofree gchar *per_minute = s->max > 0 ? g_strdup_printf("%d to %d", s->min, s->max) :
			s->min > 0 ? g_strdup_printf("%d or more", s->min) : g_strdup("any");
		g_autofree gchar *low = axis_quality_format_size((gint64)s->min * 120 * 1048576);
		g_autofree gchar *high = s->max > 0 ? axis_quality_format_size((gint64)s->max * 120 * 1048576) : NULL;
		g_autofree gchar *range = high != NULL ? g_strdup_printf("%s to %s", s->min > 0 ? low : "0", high) :
			s->min > 0 ? g_strdup_printf("%s or more", low) : g_strdup("any size");
		HtmxTr *row = htmx_tr_new();

		BROWSER_ADD(row, text_cell(name));
		BROWSER_ADD(row, text_cell(per_minute));
		BROWSER_ADD(row, text_cell(range));
		BROWSER_ADD(body, row);
	}
	BROWSER_ADD(table, body);
	BROWSER_ADD(card, table);
	BROWSER_ADD(card, axis_web_storage_text("help", "Sizes are mebibytes (MiB) per minute of the movie's or "
		"episode's runtime, as its details give it; without a runtime only the profile's limit for any release "
		"applies. A release of unknown quality has only that limit too."));
	return card;
}

static HtmxNode *
rules_card(const AxisQualityProfile *p)
{
	HtmxNode *card = axis_web_storage_card("Rules on the release name");
	guint i;

	if (p->n_rules == 0) {
		BROWSER_ADD(card, axis_web_storage_text(NULL, "No rules: releases of one quality rank by proper or repack "
			"first, then by seeders or age, then by size."));
		return card;
	}
	{
		HtmxUl *list = htmx_ul_new();

		htmx_element_add_class(HTMX_ELEMENT(list), "quality-rules");
		for (i = 0; i < p->n_rules && i < AXIS_QUALITY_MAX_RULES; i++) {
			g_autofree gchar *words = axis_quality_rule_describe(&p->rules[i]);
			HtmxLi *item = htmx_li_new();

			htmx_node_set_text_content(HTMX_NODE(item), words);
			BROWSER_ADD(list, item);
		}
		BROWSER_ADD(card, list);
	}
	BROWSER_ADD(card, axis_web_storage_text("help", "Require and reject decide whether a release is acceptable at "
		"all; prefer and avoid add to or take from its score, which ranks releases of the same quality."));
	return card;
}

static gchar *
uses_text(GPtrArray *uses)
{
	GString *joined = g_string_new(NULL);
	guint k;

	for (k = 0; uses != NULL && k < uses->len; k++)
		g_string_append_printf(joined, "%s%s", k > 0 ? "; " : "", (const gchar *)g_ptr_array_index(uses, k));
	if (joined->len == 0)
		g_string_append(joined, "nothing yet");
	return g_string_free(joined, FALSE);
}

static HtmxNode *
actions_card(
	HtmxRequest             *request,
	const AxisQualityStored *s,
	GPtrArray               *uses
){
	HtmxNode *card = axis_web_storage_card("Copy, edit or delete this profile");
	g_autofree gchar *edit = g_strdup_printf("/quality/%s/edit", s->slug);
	g_autofree gchar *delete_action = g_strdup_printf("/quality/%s/delete", s->slug);

	if (s->usable)
		BROWSER_ADD(card, axis_web_quality_copy_form(request, s));
	if (s->built_in) {
		BROWSER_ADD(card, axis_web_storage_text("help", "A built-in profile cannot be changed or deleted: copy it "
			"and edit the copy."));
		return card;
	}
	{
		HtmxDiv *row = htmx_div_new();

		htmx_element_add_class(HTMX_ELEMENT(row), "actions");
		if (s->usable)
			BROWSER_ADD(row, axis_web_browser_link(edit, "Edit this profile"));
		if (uses == NULL || uses->len == 0) {
			HtmxNode *form = axis_web_auth_post_form(request, delete_action);

			htmx_element_add_class(HTMX_ELEMENT(form), "inline-form");
			axis_web_auth_add(form, axis_web_auth_button("Delete this profile", "button-danger"));
			BROWSER_ADD(row, form);
		}
		BROWSER_ADD(card, row);
	}
	if (uses != NULL && uses->len > 0)
		BROWSER_ADD(card, axis_web_storage_text("help", "It cannot be deleted while something uses it."));
	return card;
}

static HtmxResponse *
profile_done(
	AxisWebQuality *self,
	HtmxRequest    *request,
	PageWork       *work
){
	const AxisQualityStored *s = work->stored;
	g_autofree gchar *title = g_strdup_printf("Quality profile: %s", s->name);
	g_autoptr(HtmxDiv) content = page_start(request, title, NULL);
	const AxisQualityBuiltin *builtin = s->built_in ? axis_quality_builtin(s->slug) : NULL;
	HtmxNode *card;

	if (!s->usable)
		BROWSER_ADD(content, axis_web_browser_warning(s->unusable));
	card = axis_web_storage_card("About this quality profile");
	{
		HtmxNode *facts = axis_web_browser_facts_new();
		g_autofree gchar *origin_owned = NULL;
		g_autofree gchar *changed = axis_web_format_time(s->updated_us, "never");
		g_autofree gchar *changed_by = s->updated_by != NULL ? g_strdup_printf("%s, by %s", changed, s->updated_by) :
			g_strdup(changed);
		g_autofree gchar *uses = uses_text(work->stored_uses);

		axis_web_browser_fact(facts, "Short name, for the \"Default quality profile\" setting", s->slug);
		axis_web_browser_fact(facts, "Origin", s->built_in ? "built in (read only)" : origin_text(s, &origin_owned));
		axis_web_browser_fact(facts, "Used by", uses);
		if (g_strcmp0(s->slug, work->default_slug) == 0)
			axis_web_browser_fact(facts, "Default for new titles", "yes");
		else
			axis_web_browser_fact_node(facts, "Default for new titles", axis_web_browser_link("/admin/settings#module-quality",
				"no (Settings, Quality profiles, \"Default quality profile\")"));
		axis_web_browser_fact(facts, "Last changed", s->built_in ? "with this version of Axis" : changed_by);
		BROWSER_ADD(card, facts);
		if (builtin != NULL)
			BROWSER_ADD(card, axis_web_storage_text(NULL, builtin->summary));
	}
	BROWSER_ADD(content, card);
	if (s->usable) {
		g_autoptr(GPtrArray) facts = axis_quality_profile_describe(&s->profile);
		HtmxNode *list = axis_web_browser_facts_new();
		guint i;

		card = axis_web_storage_card("What this profile accepts");
		for (i = 0; i < facts->len; i++) {
			const AxisQualityFact *f = g_ptr_array_index(facts, i);

			axis_web_browser_fact(list, f->label, f->text);
		}
		BROWSER_ADD(card, list);
		BROWSER_ADD(card, fixed_rules_text());
		BROWSER_ADD(content, card);
		BROWSER_ADD(content, sizes_card(&s->profile));
		BROWSER_ADD(content, rules_card(&s->profile));
		card = axis_web_storage_card("Try a release name");
		BROWSER_ADD(card, axis_web_quality_try_form(work->profiles, NULL, s->slug, NULL, NULL));
		BROWSER_ADD(content, card);
	}
	if (axis_web_request_is_admin(request))
		BROWSER_ADD(content, actions_card(request, s, work->stored_uses));
	{
		HtmxDetails *details = htmx_details_new();
		HtmxSummary *summary = htmx_summary_new();
		HtmxPre *pre = htmx_pre_new();
		g_autofree gchar *format = g_strdup_printf("Format %d, saved %d time%s.", s->format, s->revision,
			s->revision == 1 ? "" : "s");

		htmx_node_set_text_content(HTMX_NODE(summary), "The definition as stored");
		htmx_element_add_class(HTMX_ELEMENT(pre), "definition-text");
		htmx_node_set_text_content(HTMX_NODE(pre), s->text != NULL ? s->text : "");
		BROWSER_ADD(details, summary);
		BROWSER_ADD(details, pre);
		BROWSER_ADD(details, axis_web_storage_text("help", format));
		BROWSER_ADD(content, details);
	}
	(void)self;
	return axis_web_page_respond(request, title, HTMX_NODE(content));
}

/* ── Trying a name ──────────────────────────────────────────────── */

static HtmxNode *
reading_card(const AxisRelease *release)
{
	HtmxNode *card = axis_web_storage_card("What the release name says");
	g_autoptr(GPtrArray) facts = axis_quality_release_describe(release);
	HtmxNode *list = axis_web_browser_facts_new();
	guint i;

	for (i = 0; i < facts->len; i++) {
		const AxisQualityFact *f = g_ptr_array_index(facts, i);

		axis_web_browser_fact(list, f->label, f->text);
	}
	BROWSER_ADD(card, list);
	BROWSER_ADD(card, axis_web_storage_text("help", "The parser reads what the name says and infers nothing: a field "
		"the name does not state is not shown."));
	return card;
}

static HtmxNode *
decision_card(
	const PageWork *work
){
	const AxisQualityVerdict *v = work->verdict;
	g_autofree gchar *heading = g_strdup_printf("The decision with %s", work->stored->name);
	HtmxNode *card = axis_web_storage_card(heading);
	HtmxDiv *line = htmx_div_new();
	guint i;

	htmx_element_add_class(HTMX_ELEMENT(line), "state-line");
	BROWSER_ADD(line, axis_web_browser_badge(v->accepted ? "Accepted" : "Rejected", v->accepted ? "badge-succeeded" :
		"badge-failed"));
	{
		g_autofree gchar *score = g_strdup_printf("Score %d", v->score);
		HtmxSpan *span = htmx_span_new();

		htmx_element_add_class(HTMX_ELEMENT(span), "quality-score");
		htmx_node_set_text_content(HTMX_NODE(span), score);
		BROWSER_ADD(line, span);
	}
	BROWSER_ADD(card, line);
	if (v->accepted) {
		BROWSER_ADD(card, axis_web_storage_text(NULL, "It would be taken, unless a release above it in the ranking "
			"is found: the profile's order of qualities first, then the score, then a proper or repack over the "
			"original, then the preferred protocol, then seeders or age, then the smaller size."));
	} else {
		HtmxUl *reasons = htmx_ul_new();
		g_autofree gchar *count = g_strdup_printf("Rejected, for %u reason%s:", v->reasons->len,
			v->reasons->len == 1 ? "" : "s");

		BROWSER_ADD(card, axis_web_storage_text(NULL, count));
		htmx_element_add_class(HTMX_ELEMENT(reasons), "quality-reasons");
		for (i = 0; i < v->reasons->len; i++) {
			const AxisQualityReason *r = g_ptr_array_index(v->reasons, i);
			HtmxLi *item = htmx_li_new();
			HtmxSpan *text = htmx_span_new();

			htmx_node_set_text_content(HTMX_NODE(text), r->text);
			BROWSER_ADD(item, text);
			if (r->kind == AXIS_QUALITY_REASON_SAFETY)
				BROWSER_ADD(item, axis_web_browser_badge("Fixed rule","badge-note"));
			BROWSER_ADD(reasons, item);
		}
		BROWSER_ADD(card, reasons);
	}
	if (v->lines->len > 0) {
		HtmxUl *lines = htmx_ul_new();

		BROWSER_ADD(card, axis_web_storage_text(NULL, "How the score was made:"));
		htmx_element_add_class(HTMX_ELEMENT(lines), "quality-lines");
		for (i = 0; i < v->lines->len; i++) {
			const AxisQualityScoreLine *l = g_ptr_array_index(v->lines, i);
			HtmxLi *item = htmx_li_new();

			htmx_node_set_text_content(HTMX_NODE(item), l->text);
			BROWSER_ADD(lines, item);
		}
		BROWSER_ADD(card, lines);
	}
	if (v->notes->len > 0 || (work->players_read && work->n_devices == 0)) {
		HtmxUl *notes = htmx_ul_new();

		htmx_element_add_class(HTMX_ELEMENT(notes), "quality-notes");
		for (i = 0; i < v->notes->len; i++) {
			HtmxLi *item = htmx_li_new();

			htmx_node_set_text_content(HTMX_NODE(item), g_ptr_array_index(v->notes, i));
			BROWSER_ADD(notes, item);
		}
		if (work->players_read && work->n_devices == 0) {
			HtmxLi *item = htmx_li_new();

			htmx_node_set_text_content(HTMX_NODE(item), "No player of the household has been seen playing in the "
				"compatibility report's window, so the players preference could not be applied.");
			BROWSER_ADD(notes, item);
		}
		BROWSER_ADD(card, notes);
	}
	return card;
}

static HtmxResponse *
try_done(
	AxisWebQuality *self,
	HtmxRequest    *request,
	PageWork       *work
){
	g_autoptr(HtmxDiv) content = page_start(request, "Try a release name", NULL);
	HtmxNode *card = axis_web_storage_card("Name and quality profile to try");
	HtmxResponse *response;
	guint status = 200;

	BROWSER_ADD(content, axis_web_storage_text(NULL, "Paste a release name as an indexer lists it, choose a profile, "
		"and see what was read from the name and what the profile decides. This is how to learn what a rule "
		"does."));
	BROWSER_ADD(card, axis_web_quality_try_form(work->profiles, work->name, work->stored != NULL ? work->stored->slug :
		work->slug, work->runtime_text, work->size_text));
	if (work->input_error != NULL) {
		BROWSER_ADD(card, axis_web_auth_notice(TRUE, work->input_error));
		status = 400;
	} else if (work->stored == NULL && work->slug != NULL) {
		BROWSER_ADD(card, axis_web_auth_notice(TRUE, "There is no such quality profile."));
		status = 404;
	} else if (work->stored != NULL && !work->stored->usable) {
		BROWSER_ADD(card, axis_web_auth_notice(TRUE, work->stored->unusable));
		status = 409;
	}
	BROWSER_ADD(content, card);
	if (work->verdict != NULL) {
		BROWSER_ADD(content, reading_card(work->release));
		BROWSER_ADD(content, decision_card(work));
	}
	(void)self;
	response = axis_web_page_respond(request, "Try a release name", HTMX_NODE(content));
	if (status != 200)
		htmx_response_set_status(response, status);
	return response;
}

/* ── Handlers ───────────────────────────────────────────────────── */

static HtmxResponse *
page_done(
	AxisWebQuality *self,
	HtmxRequest    *request,
	gpointer        data
){
	PageWork *work = data;

	if (work->error != NULL)
		return axis_web_quality_error(request, work->error);
	switch (work->page) {
	case PAGE_PROFILE:
		if (work->stored == NULL)
			return not_found(request);
		return profile_done(self, request, work);
	case PAGE_TRY:
		return try_done(self, request, work);
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
	PageWork *work;

	if (!query_ok(request, "deleted", NULL))
		return bad_request(request);
	work = g_new0(PageWork, 1);
	work->page = PAGE_LIST;
	return axis_web_quality_dispatch(user_data, request, page_run, page_done, work, page_work_free);
}

static HtmxResponse *
handle_profile(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	const gchar *slug = params != NULL ? g_hash_table_lookup(params, "slug") : NULL;
	PageWork *work;

	if (!axis_quality_slug_valid(slug))
		return not_found(request);
	if (!query_ok(request, "copied saved unchanged", NULL))
		return bad_request(request);
	work = g_new0(PageWork, 1);
	work->page = PAGE_PROFILE;
	work->slug = g_strdup(slug);
	return axis_web_quality_dispatch(user_data, request, page_run, page_done, work, page_work_free);
}

/* "2.9 GiB", "850 MiB", "2.9" (GiB): bytes, or -1 with @problem set.  The
 * decimal units a release's listing may use ("3.1 GB") mean what they say. */
static gint64
parse_size(
	const gchar  *text,
	gchar       **problem
){
	static const struct
	{
		const gchar *unit;
		gdouble      scale;
	} units[] = {
		{ "", 1073741824.0 }, { "gib", 1073741824.0 }, { "mib", 1048576.0 }, { "tib", 1099511627776.0 },
		{ "gb", 1e9 }, { "mb", 1e6 }, { "tb", 1e12 }
	};
	g_autofree gchar *lower = g_ascii_strdown(text, -1);
	gchar *end = NULL;
	gdouble value;
	gdouble scale = 0;
	guint i;

	g_strstrip(lower);
	value = g_ascii_strtod(lower, &end);
	if (end == lower || !isfinite(value) || value <= 0) {
		*problem = g_strdup("The size is a number with its unit, such as 2.9 GiB or 850 MiB.");
		return -1;
	}
	while (*end == ' ')
		end++;
	for (i = 0; i < G_N_ELEMENTS(units); i++) {
		if (strcmp(end, units[i].unit) == 0)
			scale = units[i].scale;
	}
	if (scale == 0) {
		*problem = g_strdup("The size is a number with its unit, such as 2.9 GiB or 850 MiB.");
		return -1;
	}
	if (value * scale > 10.0 * 1099511627776.0) {
		*problem = g_strdup("The size is at most 10 TiB.");
		return -1;
	}
	return (gint64)llround(value * scale);
}

static HtmxResponse *
handle_try(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	static const gchar *const names[] = { "name", "profile", "runtime", "size", NULL };
	AxisWebQuality *self = user_data;
	const gchar *name = htmx_request_get_query_param(request, "name");
	const gchar *profile = htmx_request_get_query_param(request, "profile");
	const gchar *runtime = htmx_request_get_query_param(request, "runtime");
	const gchar *size = htmx_request_get_query_param(request, "size");
	PageWork *work;

	if (!query_ok(request, NULL, names))
		return bad_request(request);
	work = g_new0(PageWork, 1);
	work->page = PAGE_TRY;
	work->size = -1;
	/* A name as typed, valid UTF-8, cut to the parser's limit. */
	if (name != NULL && name[0] != '\0') {
		g_autofree gchar *valid = g_utf8_make_valid(name, MIN(strlen(name), AXIS_WEB_QUALITY_NAME_MAX * 2));
		const gchar *end = valid;

		while (*end != '\0' && (gsize)(g_utf8_next_char(end) - valid) <= AXIS_WEB_QUALITY_NAME_MAX)
			end = g_utf8_next_char(end);
		work->name = g_strndup(valid, (gsize)(end - valid));
		g_strstrip(work->name);
		if (work->name[0] == '\0')
			g_clear_pointer(&work->name, g_free);
	}
	if (profile != NULL && profile[0] != '\0') {
		if (!axis_quality_slug_valid(profile)) {
			page_work_free(work);
			return not_found(request);
		}
		work->slug = g_strdup(profile);
	} else {
		work->slug = axis_quality_store_default_slug(self->store);
	}
	if (runtime != NULL && runtime[0] != '\0') {
		guint64 minutes = 0;

		work->runtime_text = g_strdup(runtime);
		if (!g_ascii_string_to_unsigned(runtime, 10, 1, 100000, &minutes, NULL))
			work->input_error = g_strdup("The runtime is a whole number of minutes, such as 97.");
		else
			work->runtime = (gint)minutes;
	}
	if (size != NULL && size[0] != '\0' && work->input_error == NULL) {
		work->size_text = g_strdup(size);
		work->size = parse_size(size, &work->input_error);
	} else if (size != NULL && size[0] != '\0') {
		work->size_text = g_strdup(size);
	}
	return axis_web_quality_dispatch(self, request, page_run, page_done, work, page_work_free);
}

/* ── Actions ────────────────────────────────────────────────────── */

typedef enum
{
	ACTION_COPY,
	ACTION_DELETE
} ActionId;

typedef struct
{
	ActionId   action;
	gchar     *slug;
	gchar     *name;
	gchar     *user;
	gchar     *created;
	GError    *error;
} ActionWork;

static void
action_work_free(gpointer data)
{
	ActionWork *work = data;

	g_free(work->slug);
	g_free(work->name);
	g_free(work->user);
	g_free(work->created);
	g_clear_error(&work->error);
	g_free(work);
}

static void
action_run(
	AxisWebQuality *self,
	gpointer        data
){
	ActionWork *work = data;

	if (work->action == ACTION_DELETE) {
		axis_quality_store_delete(self->store, work->slug, NULL, &work->error);
		return;
	}
	/* The default name is the original's, "Copy of" before it. */
	if (work->name == NULL) {
		g_autoptr(AxisQualityStored) source = NULL;

		if (!axis_quality_store_get(self->store, work->slug, &source, &work->error))
			return;
		if (source == NULL) {
			g_set_error_literal(&work->error, AXIS_QUALITY_ERROR, AXIS_QUALITY_ERROR_NOT_FOUND,
				"There is no such quality profile.");
			return;
		}
		{
			g_autofree gchar *raw = g_strdup_printf("Copy of %s", source->name);

			work->name = axis_quality_clean_name(raw);
		}
	}
	axis_quality_store_copy(self->store, work->slug, work->name, work->user, &work->created, &work->error);
}

static HtmxResponse *
action_done(
	AxisWebQuality *self,
	HtmxRequest    *request,
	gpointer        data
){
	ActionWork *work = data;
	g_autofree gchar *location = NULL;

	if (work->error != NULL)
		return axis_web_quality_error(request, work->error);
	if (work->action == ACTION_DELETE) {
		axis_web_storage_audit(request, "quality profile %s deleted", work->slug);
		return axis_web_auth_redirect(request, "/quality?done=deleted");
	}
	axis_web_storage_audit(request, "quality profile %s copied to %s", work->slug, work->created);
	location = g_strdup_printf("/quality/%s?done=copied", work->created);
	(void)self;
	return axis_web_auth_redirect(request, location);
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

	if (!axis_quality_slug_valid(slug))
		return not_found(request);
	work = g_new0(ActionWork, 1);
	work->action = ACTION_COPY;
	work->slug = g_strdup(slug);
	work->user = g_strdup(axis_web_request_get_username(request));
	/* A name is a person's text: cleaned and capped, else the default. */
	if (name != NULL && name[0] != '\0') {
		work->name = axis_quality_clean_name(name);
		if (work->name == NULL) {
			action_work_free(work);
			return axis_web_error_respond(request, 400, "Bad request", "The name is not text that can be shown.");
		}
	}
	return axis_web_quality_dispatch(user_data, request, action_run, action_done, work, action_work_free);
}

static HtmxResponse *
handle_delete(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	const gchar *slug = params != NULL ? g_hash_table_lookup(params, "slug") : NULL;
	ActionWork *work;

	if (!axis_quality_slug_valid(slug))
		return not_found(request);
	work = g_new0(ActionWork, 1);
	work->action = ACTION_DELETE;
	work->slug = g_strdup(slug);
	work->user = g_strdup(axis_web_request_get_username(request));
	return axis_web_quality_dispatch(user_data, request, action_run, action_done, work, action_work_free);
}

void
axis_web_quality_register_pages(
	AxisWebQuality *self,
	AxisWeb        *web
){
	axis_web_get(web, "/quality", handle_list, self);
	axis_web_get(web, "/quality/try", handle_try, self);
	axis_web_get(web, "/quality/:slug", handle_profile, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/quality/:slug/copy", AXIS_WEB_ROUTE_ADMIN, handle_copy, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/quality/:slug/delete", AXIS_WEB_ROUTE_ADMIN, handle_delete, self);
}
