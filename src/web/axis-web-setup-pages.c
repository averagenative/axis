/*
 * axis-web-setup-pages.c - the setup guide's overview and its steps' pages
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Every page has the steps down the left (across the top on a phone), each
 * with its mark, and the open page beside them.  A step's page says what the
 * thing is and why Axis needs it, what the layer's own check says, where the
 * key or address is found, and then the real form, built by the function of
 * the page the thing belongs to and posting to that page's handler with the
 * step as its "return".  Messages after a redirect come from fixed words.
 */

#include "web/axis-web-setup-private.h"

#include <string.h>

#include "download/axis-download-sabnzbd.h"
#include "import/axis-import.h"
#include "indexer/axis-indexer-newznab.h"
#include "indexer/axis-indexer-prowlarr.h"
#include "meta/axis-meta-tmdb.h"
#include "meta/axis-meta-tmdb-parse.h"
#include "web/axis-web-auth-private.h"
#include "web/axis-web-downloads.h"
#include "web/axis-web-identify.h"
#include "web/axis-web-imports.h"
#include "web/axis-web-indexers.h"
#include "web/axis-web-notice.h"
#include "web/axis-web-private.h"
#include "web/axis-web-settings.h"
#include "web/axis-web-storage.h"

#define ADD axis_web_setup_add

/* The services' own pages, outside Axis. */
#define TMDB_API_SETTINGS "https://www.themoviedb.org/settings/api"
#define TMDB_SIGNUP       "https://www.themoviedb.org/signup"
#define PROWLARR_DOCS     "https://wiki.servarr.com/prowlarr"
#define SABNZBD_DOCS      "https://sabnzbd.org/wiki/"
#define DELUGE_DOCS       "https://deluge.readthedocs.io/en/latest/"
#define QBITTORRENT_DOCS  "https://github.com/qbittorrent/qBittorrent/wiki"
#define PLEX_TOKEN_HELP   "https://support.plex.tv/articles/204059436-finding-an-authentication-token-x-plex-token/"
#define JELLYFIN_DOCS     "https://jellyfin.org/docs/"

/* ── Pieces ─────────────────────────────────────────────────────── */

static HtmxNode *
card_new(const gchar *heading)
{
	HtmxSection *card = htmx_section_new();
	HtmxHeading *title = htmx_h2_new();

	htmx_element_add_class(HTMX_ELEMENT(card), "card");
	htmx_node_set_text_content(HTMX_NODE(title), heading);
	ADD(card, title);
	return HTMX_NODE(card);
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
span_text(
	const gchar *css,
	const gchar *text
){
	HtmxSpan *span = htmx_span_new();

	if (css != NULL)
		htmx_element_add_class(HTMX_ELEMENT(span), css);
	htmx_node_set_text_content(HTMX_NODE(span), text);
	return HTMX_NODE(span);
}

/* A paragraph of text with links in it: text, link, text, link, ...; the
 * links are (href, words) pairs, an outbound one when its href starts with
 * "https://". */
static HtmxNode *
line_with_links(
	const gchar *css,
	const gchar *first,
	...
) G_GNUC_NULL_TERMINATED;

static HtmxNode *
line_with_links(
	const gchar *css,
	const gchar *first,
	...
){
	HtmxP *p = htmx_p_new();
	const gchar *text = first;
	va_list ap;

	if (css != NULL)
		htmx_element_add_class(HTMX_ELEMENT(p), css);
	va_start(ap, first);
	while (text != NULL) {
		const gchar *href;
		const gchar *words;

		if (text[0] != '\0')
			ADD(p, span_text(NULL, text));
		href = va_arg(ap, const gchar *);
		if (href == NULL)
			break;
		words = va_arg(ap, const gchar *);
		ADD(p, g_str_has_prefix(href, "https://") ? axis_web_setup_link_out(href, words) : link_to(href, words));
		text = va_arg(ap, const gchar *);
	}
	va_end(ap);
	return HTMX_NODE(p);
}

/* An ordered list of steps to follow; each item a node. */
static HtmxNode *
how_list(void)
{
	HtmxOl *list = htmx_ol_new();

	htmx_element_add_class(HTMX_ELEMENT(list), "guide-how");
	return HTMX_NODE(list);
}

static void
how_add(
	HtmxNode *list,
	HtmxNode *content
){
	HtmxLi *item = htmx_li_new();

	ADD(item, content);
	ADD(list, item);
}

static void
how_add_text(
	HtmxNode    *list,
	const gchar *text
){
	ADD(list, htmx_li_new_with_text(text));
}

/* The word and badge of a step's state. */
static const gchar *
state_word(
	const AxisSetupState *s,
	AxisSetupStep         step
){
	if (s->done)
		return "Done";
	if (s->off)
		return "Module off";
	if (s->dismissed)
		return "Not used";
	if (s->skipped)
		return "Skipped for now";
	return axis_setup_step_optional(step) ? "Optional" : "Needed";
}

static const gchar *
state_class(
	const AxisSetupState *s,
	AxisSetupStep         step
){
	if (s->done)
		return "done";
	if (s->off)
		return "off";
	if (s->dismissed)
		return "dismissed";
	if (s->skipped)
		return "skipped";
	return axis_setup_step_optional(step) ? "optional" : "needed";
}

static HtmxNode *
state_badge(
	const AxisSetupState *s,
	AxisSetupStep         step
){
	HtmxSpan *badge = htmx_span_new();
	g_autofree gchar *css = g_strdup_printf("guide-badge-%s", state_class(s, step));

	htmx_element_add_class(HTMX_ELEMENT(badge), "badge");
	htmx_element_add_class(HTMX_ELEMENT(badge), css);
	if (s->done)
		htmx_element_add_class(HTMX_ELEMENT(badge), "badge-succeeded");
	else if (!s->off && !s->dismissed && !s->skipped && !axis_setup_step_optional(step))
		htmx_element_add_class(HTMX_ELEMENT(badge), "badge-warn");
	htmx_node_set_text_content(HTMX_NODE(badge), state_word(s, step));
	return HTMX_NODE(badge);
}

/* The mark beside a step's name: drawn by the stylesheet, its meaning in
 * words for a screen reader. */
static HtmxNode *
state_mark(
	const AxisSetupState *s,
	AxisSetupStep         step
){
	HtmxSpan *mark = htmx_span_new();
	g_autofree gchar *css = g_strdup_printf("guide-mark-%s", state_class(s, step));

	htmx_element_add_class(HTMX_ELEMENT(mark), "guide-mark");
	htmx_element_add_class(HTMX_ELEMENT(mark), css);
	htmx_element_set_attribute(HTMX_ELEMENT(mark), "aria-hidden", "true");
	return HTMX_NODE(mark);
}

/* ── The frame: the steps beside the page ───────────────────────── */

static HtmxNode *
steps_nav(
	const AxisSetupFigures *f,
	gint                    current
){
	HtmxNav *nav = htmx_nav_new();
	HtmxOl *list = htmx_ol_new();
	HtmxA *overview = htmx_a_new_with_href(AXIS_WEB_SETUP_PATH);
	guint i;

	htmx_element_add_class(HTMX_ELEMENT(nav), "guide-steps");
	htmx_element_set_attribute(HTMX_ELEMENT(nav), "aria-label", "Setup guide steps");
	htmx_element_add_class(HTMX_ELEMENT(overview), "guide-overview-link");
	htmx_node_set_text_content(HTMX_NODE(overview), "All steps");
	if (current < 0)
		htmx_element_set_attribute(HTMX_ELEMENT(overview), "aria-current", "page");
	ADD(nav, overview);
	htmx_element_add_class(HTMX_ELEMENT(list), "guide-step-list");
	for (i = 0; i < AXIS_SETUP_N_STEPS; i++) {
		const AxisSetupState *s = &f->states[i];
		HtmxLi *item = htmx_li_new();
		HtmxA *link = htmx_a_new_with_href(axis_setup_step_path(i));
		g_autofree gchar *number = g_strdup_printf("%u", i + 1);
		g_autofree gchar *hidden = g_strdup_printf(", %s", state_word(s, i));
		g_autofree gchar *css = g_strdup_printf("guide-step-%s", state_class(s, i));

		htmx_element_add_class(HTMX_ELEMENT(item), "guide-step");
		htmx_element_add_class(HTMX_ELEMENT(item), css);
		if ((gint)i == current)
			htmx_element_set_attribute(HTMX_ELEMENT(link), "aria-current", "step");
		ADD(link, state_mark(s, i));
		ADD(link, span_text("guide-step-number", number));
		ADD(link, span_text("guide-step-name", axis_setup_step_label(i)));
		ADD(link, span_text("visually-hidden", hidden));
		ADD(item, link);
		ADD(list, item);
	}
	ADD(nav, list);
	return HTMX_NODE(nav);
}

/* The page: the steps, and @body beside them. */
static HtmxNode *
frame(
	const AxisSetupFigures *f,
	gint                    current,
	HtmxNode               *body
){
	HtmxDiv *guide = htmx_div_new();

	htmx_element_add_class(HTMX_ELEMENT(guide), "guide");
	ADD(guide, steps_nav(f, current));
	htmx_element_add_class(HTMX_ELEMENT(body), "guide-body");
	ADD(guide, body);
	return HTMX_NODE(guide);
}

/* What a redirect's word means on @step's page (-1: the overview). */
static const gchar *
done_text(
	gint         step,
	const gchar *done
){
	if (done == NULL)
		return NULL;
	if (strcmp(done, "saved") == 0)
		return step == AXIS_SETUP_TITLES ? "The token has been saved." : step == AXIS_SETUP_QUALITY ?
			"The default quality profile has been saved." : step == AXIS_SETUP_PLAYERS ? "The connection has been saved." :
			"Saved.";
	if (strcmp(done, "saved-restart") == 0)
		return "Saved. It takes effect after the next restart.";
	if (strcmp(done, "unchanged") == 0)
		return "Nothing needed saving: it was already so.";
	if (strcmp(done, "added") == 0)
		return step == AXIS_SETUP_LIBRARY ? "The disk has been added." : step == AXIS_SETUP_INDEXERS ?
			"The indexer has been added. Test it now: Axis has not asked it anything yet." : step == AXIS_SETUP_CLIENTS ?
			"The download client has been added. Test it now, then tell Axis where its files appear." : "Added.";
	if (strcmp(done, "tested") == 0)
		return "Tested: what it said is below.";
	if (strcmp(done, "map-added") == 0)
		return "The path mapping has been added.";
	if (strcmp(done, "map-removed") == 0)
		return "The path mapping has been removed.";
	if (strcmp(done, "root-added") == 0)
		return "The library folder has been added.";
	if (strcmp(done, "run") == 0)
		return "Identification has been queued. It runs in the background; this page shows what it found once it "
			"has.";
	if (strcmp(done, "skipped") == 0)
		return "Put off until later: the guide comes back to that step once the others are done.";
	if (strcmp(done, "dismissed") == 0)
		return "Marked as not used: the guide and the dashboard no longer ask for that step. Its page can ask for it "
			"again.";
	if (strcmp(done, "restored") == 0)
		return "The guide asks for this step again.";
	return NULL;
}

/* The settings form of @module_id, as the Settings page builds it, with only
 * @keys and coming back to @step; %NULL when the module is off. */
static HtmxNode *
settings_form(
	AxisWebSetup                *self,
	HtmxRequest                 *request,
	const gchar                 *module_id,
	const gchar *const          *keys,
	const gchar                 *button,
	AxisSetupStep                step,
	const gchar                 *choice_key,
	const AxisWebSettingsChoice *choices,
	guint                        n_choices
){
	AxisModuleRegistry *registry = axis_web_setup_registry(self);
	gint index = registry != NULL ? axis_module_registry_find(registry, module_id) : -1;
	const AxisSettingInfo *table;
	AxisWebSettingsForm form;

	if (index < 0 || !axis_module_registry_is_enabled(registry, module_id) ||
	    (table = axis_module_registry_get_settings(registry, (guint)index)) == NULL)
		return NULL;
	memset(&form, 0, sizeof(form));
	form.keys = keys;
	form.button = button;
	form.return_path = axis_setup_step_path(step);
	form.choice_key = choice_key;
	form.choices = choices;
	form.n_choices = n_choices;
	return axis_web_settings_form_new(self->settings, request, module_id, axis_module_registry_get_name(registry,
		(guint)index), table, &form);
}

/* A step's actions: put it off, say it is not used (or ask for it again),
 * and the way on. */
static HtmxNode *
step_actions(
	HtmxRequest          *request,
	const AxisSetupState *s,
	AxisSetupStep         step,
	AxisWebNotice        *notice
){
	HtmxDiv *row = htmx_div_new();
	HtmxDiv *choices = htmx_div_new();
	g_autofree gchar *base = g_strdup(axis_setup_step_path(step));

	htmx_element_set_id(HTMX_ELEMENT(row), "step-actions");
	axis_web_notice_put(notice, HTMX_NODE(row), "step-actions");
	htmx_element_add_class(HTMX_ELEMENT(row), "guide-actions");
	htmx_element_add_class(HTMX_ELEMENT(choices), "actions");
	if (s->dismissed) {
		g_autofree gchar *action = g_strconcat(base, "/restore", NULL);
		HtmxNode *form = axis_web_auth_post_form(request, action);

		htmx_element_add_class(HTMX_ELEMENT(form), "inline-form");
		axis_web_auth_add(form, axis_web_auth_button("Ask for this step again", NULL));
		ADD(choices, form);
	} else if (!s->done) {
		g_autofree gchar *skip = g_strconcat(base, "/skip", NULL);
		g_autofree gchar *dismiss = g_strconcat(base, "/dismiss", NULL);
		HtmxNode *later = axis_web_auth_post_form(request, skip);
		HtmxNode *never = axis_web_auth_post_form(request, dismiss);

		htmx_element_add_class(HTMX_ELEMENT(later), "inline-form");
		htmx_element_add_class(HTMX_ELEMENT(never), "inline-form");
		axis_web_auth_add(later, axis_web_auth_button("Skip this step for now", NULL));
		axis_web_auth_add(never, axis_web_auth_button("Mark this step as not used", NULL));
		ADD(choices, later);
		ADD(choices, never);
	}
	ADD(row, choices);
	if ((guint)step + 1 < AXIS_SETUP_N_STEPS) {
		g_autofree gchar *words = g_strdup_printf("Next: %s", axis_setup_step_label(step + 1));
		HtmxNode *next = link_to(axis_setup_step_path(step + 1), words);

		htmx_element_add_class(HTMX_ELEMENT(next), "guide-next");
		ADD(row, next);
	} else {
		HtmxNode *next = link_to(AXIS_WEB_SETUP_PATH, "Back to all steps");

		htmx_element_add_class(HTMX_ELEMENT(next), "guide-next");
		ADD(row, next);
	}
	return HTMX_NODE(row);
}

/* "Where it stands": the step's mark and what its check found. */
static HtmxNode *
state_card(
	const AxisSetupState *s,
	AxisSetupStep         step
){
	HtmxNode *card = card_new("Status of this step");
	HtmxP *line = htmx_p_new();

	htmx_element_add_class(HTMX_ELEMENT(card), "guide-state");
	htmx_element_add_class(HTMX_ELEMENT(line), "guide-state-line");
	ADD(line, state_badge(s, step));
	ADD(line, span_text("guide-state-text", s->summary != NULL ? s->summary : ""));
	ADD(card, line);
	return card;
}

/* A line of one thing a step looked at: its badge, its sentence, and (in
 * @what, borrowed) the block under the sentence, for what can be done to it. */
static HtmxNode *
item_line(
	gboolean      good,
	const gchar  *word,
	const gchar  *text,
	HtmxNode    **what_out
){
	HtmxDiv *row = htmx_div_new();
	HtmxSpan *badge = htmx_span_new();
	HtmxDiv *what = htmx_div_new();

	htmx_element_add_class(HTMX_ELEMENT(row), "guide-item");
	htmx_element_add_class(HTMX_ELEMENT(badge), "badge");
	htmx_element_add_class(HTMX_ELEMENT(badge), good ? "badge-succeeded" : "badge-warn");
	htmx_node_set_text_content(HTMX_NODE(badge), word);
	htmx_element_add_class(HTMX_ELEMENT(what), "guide-item-what");
	ADD(what, axis_web_setup_text("guide-item-text", text));
	ADD(row, badge);
	*what_out = HTMX_NODE(what);
	ADD(row, what);
	return HTMX_NODE(row);
}

/* The block under a state card's sentence, for what can be done with its
 * one item (borrowed; added to @card). */
static HtmxNode *
solo_block(HtmxNode *card)
{
	HtmxDiv *block = htmx_div_new();

	htmx_element_add_class(HTMX_ELEMENT(block), "guide-solo");
	ADD(card, block);
	return HTMX_NODE(block);
}

/* The answer of a check run by this request, or %NULL. */
static HtmxNode *
check_notice(const AxisSetupFigures *f)
{
	HtmxNode *notice;

	if (!f->checked || f->check_text == NULL)
		return NULL;
	notice = axis_web_auth_notice(!f->check_ok, f->check_text);
	htmx_element_add_class(HTMX_ELEMENT(notice), "guide-check");
	return notice;
}

/* ── Step 1: titles and artwork ─────────────────────────────────── */

static void
step_titles(
	AxisWebSetup     *self,
	HtmxRequest      *request,
	AxisSetupFigures *f,
	HtmxNode         *body
){
	const AxisSetupState *s = &f->states[AXIS_SETUP_TITLES];
	g_autoptr(GPtrArray) keys = g_ptr_array_new_with_free_func(g_free);
	gboolean tmdb = FALSE;
	gboolean key_set = FALSE;
	const gchar *label = "TMDb";
	HtmxNode *card;
	HtmxNode *form;
	HtmxNode *answer;
	guint i;

	for (i = 0; f->providers != NULL && i < f->providers->len; i++) {
		const AxisMetaProviderStatus *status = &g_array_index(f->providers, AxisMetaProviderStatus, i);

		if (!status->needs_key)
			continue;
		if (keys->len == 0)
			label = status->info->label;
		g_ptr_array_add(keys, g_strdup_printf("%s.%s_key", AXIS_META_MODULE_ID, status->info->name));
		tmdb = tmdb || strcmp(status->info->name, AXIS_META_TMDB_NAME) == 0;
		key_set = key_set || status->key_set;
	}
	g_ptr_array_add(keys, NULL);

	card = state_card(s, AXIS_SETUP_TITLES);
	answer = check_notice(f);
	if (answer != NULL)
		ADD(card, answer);
	if (key_set && !s->off) {
		HtmxNode *check = axis_web_auth_post_form(request, AXIS_WEB_SETUP_PATH "/titles/check");

		htmx_element_add_class(HTMX_ELEMENT(check), "inline-form");
		axis_web_auth_add(check, axis_web_auth_button("Check the token", NULL));
		ADD(card, axis_web_setup_text("help", "The check looks up one movie, " AXIS_WEB_SETUP_CHECK_TITLE
			", as the Movie and TV details page's Check a key with a lookup does, and says what came back."));
		ADD(card, check);
	}
	ADD(body, card);
	if (s->off) {
		ADD(body, line_with_links("help", "Switch Metadata on in ", "/admin/modules", "Modules", " first.", NULL));
		return;
	}

	card = card_new(tmdb ? "Get the token" : "Get the key");
	if (tmdb) {
		HtmxNode *how = how_list();

		how_add(how, line_with_links(NULL, "Sign in at TMDb, or ", TMDB_SIGNUP, "make an account", " (it costs "
			"nothing), and open ", TMDB_API_SETTINGS, "the API page of your account settings", ".", NULL));
		how_add_text(how, "If TMDb asks, create an API key for personal use. The page then shows two values.");
		how_add_text(how, "Copy the API Read Access Token: the long one, which starts with eyJ. Not the API Key, the "
			"short one of 32 letters and digits: Axis refuses that one.");
		how_add_text(how, "Paste it below and save it, then check it.");
		ADD(card, how);
	} else {
		g_autofree gchar *text = g_strdup_printf("%s takes a key of its own: give it below, then check it.", label);

		ADD(card, axis_web_setup_text(NULL, text));
	}
	form = settings_form(self, request, AXIS_META_MODULE_ID, (const gchar *const *)keys->pdata, tmdb ?
		"Save the token" : "Save the key", AXIS_SETUP_TITLES, NULL, NULL, 0);
	if (form != NULL)
		ADD(card, form);
	if (tmdb)
		ADD(card, axis_web_setup_guide_token());
	ADD(body, card);
}

/* ── Step 2: where the library is ───────────────────────────────── */

static const AxisVolumeInfo *
volume_of(
	const AxisSetupFigures *f,
	gint64                  id
){
	guint i;

	for (i = 0; f->volumes != NULL && i < f->volumes->len; i++) {
		const AxisVolumeInfo *volume = g_ptr_array_index(f->volumes, i);

		if (volume->id == id)
			return volume;
	}
	return NULL;
}

static HtmxNode *
library_table(const AxisSetupFigures *f)
{
	static const AxisWebColumn columns[] = {
		{ "Library folder", AXIS_WEB_COL_NAME },
		{ "Holds", AXIS_WEB_COL_TEXT },
		{ "Disk", AXIS_WEB_COL_TEXT },
		{ "Path", AXIS_WEB_COL_WIDE },
		{ "Disk state", AXIS_WEB_COL_STATE },
		{ NULL, 0 }
	};
	HtmxNode *table = axis_web_table_new("setup-roots", columns);
	HtmxTbody *body = htmx_tbody_new();
	guint i;

	for (i = 0; i < f->roots->len; i++) {
		const AxisRootInfo *root = g_ptr_array_index(f->roots, i);
		const AxisVolumeInfo *volume = volume_of(f, root->volume_id);
		g_autofree gchar *where = volume == NULL ? g_strdup(root->rel_path) : root->rel_path[0] == '\0' ?
			g_strdup(volume->path) : g_strdup_printf("%s/%s", volume->path, root->rel_path);
		HtmxTr *row = htmx_tr_new();
		HtmxTd *state = htmx_td_new();
		HtmxTd *dir = htmx_td_new();
		HtmxSpan *badge = htmx_span_new();
		g_autofree gchar *css = g_strdup_printf("state-%s", volume != NULL ?
			axis_volume_state_to_string(volume->state) : "offline");

		ADD(row, htmx_td_new_with_text(root->name));
		ADD(row, htmx_td_new_with_text(axis_media_type_label(root->media_type)));
		ADD(row, htmx_td_new_with_text(volume != NULL ? volume->name : "unknown"));
		ADD(dir, htmx_code_new_with_text(where));
		ADD(row, dir);
		htmx_element_add_class(HTMX_ELEMENT(badge), "badge");
		htmx_element_add_class(HTMX_ELEMENT(badge), css);
		htmx_node_set_text_content(HTMX_NODE(badge), volume != NULL ? axis_volume_state_label(volume->state) :
			"Unknown");
		ADD(state, badge);
		ADD(row, state);
		ADD(body, row);
	}
	ADD(table, body);
	return table;
}

static void
step_library(
	AxisWebSetup     *self,
	HtmxRequest      *request,
	AxisSetupFigures *f,
	HtmxNode         *body
){
	const AxisSetupState *s = &f->states[AXIS_SETUP_LIBRARY];
	const gchar *back = axis_setup_step_path(AXIS_SETUP_LIBRARY);
	HtmxNode *card = state_card(s, AXIS_SETUP_LIBRARY);

	if (f->roots != NULL && f->roots->len > 0)
		ADD(card, library_table(f));
	ADD(body, card);
	if (!f->storage_on) {
		ADD(body, line_with_links("help", "Switch Storage on in ", "/admin/modules", "Modules", " first.", NULL));
		return;
	}

	card = card_new("1. Add a disk");
	ADD(card, axis_web_setup_text(NULL, "Give the directory a disk is mounted at, as Axis sees it: inside a container, "
		"the container's own path, such as /media/disk1. Axis looks at it first and shows you what is there; "
		"nothing is written until you confirm on the next page."));
	ADD(card, axis_web_setup_text(NULL, "Adding a disk changes nothing on it but one small file at its top, "
		".axis-volume, holding a random identifier, and only when it has none. By that file Axis knows the disk "
		"wherever it is mounted, so a disk that is not mounted shows as offline, never as empty. It is written even "
		"while storage is read-only, after you confirm. A disk mounted read-only can be added too: Axis then "
		"recognises it by its filesystem's identifier and writes the file the first time it finds the disk "
		"mounted read-write."));
	ADD(card, axis_web_storage_add_volume_form(request, NULL, NULL, back));
	ADD(body, card);

	card = card_new(f->films && f->series ? "2. Add a library folder for Movies and one for TV" :
		"2. Add a library folder");
	ADD(card, axis_web_setup_text(NULL, f->films && f->series ? "A library folder is a folder on a disk that holds one "
		"kind of media: Movies or TV. Point one at the folder your movies are in (or an empty one for a new library), "
		"and one at your series. Adding a library folder writes nothing." : "A library folder is a folder on a disk "
		"that holds one kind of media. Point it at the folder your titles are in, or an empty one for a new library. "
		"Adding a library folder writes nothing."));
	if (f->volumes == NULL || f->volumes->len == 0)
		ADD(card, axis_web_setup_text("help", "Add a disk first: a library folder is a folder on one."));
	else
		ADD(card, axis_web_storage_add_root_form(request, f->volumes, back));
	ADD(card, axis_web_setup_guide_one_disk());
	ADD(body, card);
}

/* ── Step 3: indexers ───────────────────────────────────────────── */

static void
step_indexers(
	AxisWebSetup     *self,
	HtmxRequest      *request,
	AxisSetupFigures *f,
	HtmxNode         *body
){
	const AxisSetupState *s = &f->states[AXIS_SETUP_INDEXERS];
	const gchar *back = axis_setup_step_path(AXIS_SETUP_INDEXERS);
	HtmxNode *card = state_card(s, AXIS_SETUP_INDEXERS);
	guint n = f->sources != NULL ? f->sources->len : 0;
	HtmxNode *how;
	guint i;

	/* One source: what to do with it under the sentence; several: a line
	 * of each. */
	for (i = 0; i < n; i++) {
		const AxisIndexerSourceInfo *source = g_ptr_array_index(f->sources, i);
		g_autofree gchar *text = axis_web_setup_source_sentence(source);
		g_autofree gchar *href = g_strdup_printf("/indexers/%" G_GINT64_FORMAT, source->id);
		gboolean good = source->enabled && source->state == AXIS_INDEXER_SOURCE_OK;
		g_autofree gchar *link_words = g_strdup_printf("Indexer page for %s", source->name);
		HtmxNode *what = NULL;
		HtmxDiv *acts = htmx_div_new();
		const AxisIndexerKindInfo *info = axis_indexers_find_kind(self->indexers, source->kind);
		gboolean site = info != NULL && info->site;
		HtmxNode *edit = axis_web_setup_more(site ? "Change this indexer's address" :
			"Change this indexer's address or key");

		if (n == 1)
			what = solo_block(card);
		else
			ADD(card, item_line(good, good ? "Answered" : !source->enabled ? "Off" :
				source->state == AXIS_INDEXER_SOURCE_UNCHECKED ? "Not tested" : "Problem", text, &what));
		htmx_element_add_class(HTMX_ELEMENT(acts), "actions");
		ADD(acts, axis_web_indexers_test_form(request, source->id, "Test this indexer now", "button-small", back));
		ADD(acts, link_to(href, link_words));
		ADD(what, acts);
		/* A refused key is fixed here: open. A public site has no key. */
		if (!site && (source->state == AXIS_INDEXER_SOURCE_REFUSED || !source->key_set))
			htmx_element_set_attribute(HTMX_ELEMENT(edit), "open", "open");
		ADD(edit, axis_web_indexers_edit_form(self->indexers, request, source, back));
		ADD(what, edit);
	}
	ADD(body, card);
	if (s->off) {
		ADD(body, line_with_links("help", "Switch Indexers on in ", "/admin/modules", "Modules", " first.", NULL));
		return;
	}

	/* A Usenet indexer, asked directly. */
	card = card_new("Add a Usenet indexer");
	how = how_list();
	how_add_text(how, "The address: the indexer's API address, often https://api. and its name, such as "
		"https://api.example.com. Nothing after the host; Axis asks its /api there.");
	how_add_text(how, "The key: on the indexer's site, in your profile or on its API page (it may be called the API "
		"key or the RSS key). It goes only to that indexer's address.");
	how_add_text(how, "The categories: leave them as they are (2000, all movies; 5000, all TV) unless you know the "
		"indexer's own. Test it after adding it: the test reads what it can search by.");
	ADD(card, how);
	{
		HtmxNode *more = n > 0 ? axis_web_setup_more("Add a Usenet indexer") : NULL;
		HtmxNode *form = axis_web_indexers_add_form(self->indexers, request, AXIS_INDEXER_NEWZNAB_NAME, back);

		if (more != NULL) {
			ADD(more, form);
			ADD(card, more);
		} else {
			ADD(card, form);
		}
	}
	ADD(body, card);

	card = card_new("Or connect Prowlarr");
	how = how_list();
	how_add_text(how, "The address: the one you open Prowlarr at, as Axis can reach it. Between containers on one "
		"network that is its name and port, such as http://prowlarr:9696; otherwise the machine's address and port.");
	how_add_text(how, "The base path, only if Prowlarr is served under a path (its URL Base, set behind a reverse "
		"proxy), such as /prowlarr. Otherwise leave it empty.");
	how_add_text(how, "The key: in Prowlarr, open Settings, then General. The API Key is under Security.");
	ADD(card, how);
	for (i = 0; f->sources != NULL && i < f->sources->len; i++) {
		const AxisIndexerSourceInfo *source = g_ptr_array_index(f->sources, i);
		g_autofree gchar *words = NULL;
		HtmxNode *link;

		if (strcmp(source->kind, AXIS_INDEXER_PROWLARR_NAME) != 0)
			continue;
		words = g_strdup_printf("Open %s's General settings", source->name);
		link = axis_web_setup_link_to_service(source->address, source->base_path, "/settings/general", words);
		if (link != NULL) {
			HtmxP *p = htmx_p_new();

			htmx_element_add_class(HTMX_ELEMENT(p), "guide-service-link");
			ADD(p, link);
			ADD(card, p);
		}
	}
	ADD(card, line_with_links("help", "More in ", PROWLARR_DOCS, "Prowlarr's documentation", ".", NULL));
	{
		HtmxNode *more = axis_web_setup_more("Connect Prowlarr");

		ADD(more, axis_web_indexers_add_form(self->indexers, request, AXIS_INDEXER_PROWLARR_NAME, back));
		ADD(card, more);
	}
	ADD(card, line_with_links("help", "Torrent feeds of Jackett's (Torznab) and the public torrent sites YTS, EZTV and "
		"The Pirate Bay, which need no key, are added on the ", "/indexers", "Indexers page", ".", NULL));
	ADD(body, card);
}

/* ── Step 4: download clients ───────────────────────────────────── */

/* Whether the library roots of the types that are on share @volume_id, in
 * a sentence. */
static gchar *
disk_verdict(
	const AxisSetupFigures *f,
	gint64                  volume_id
){
	g_autoptr(GString) same = g_string_new(NULL);
	g_autoptr(GString) other = g_string_new(NULL);
	guint i;

	for (i = 0; f->roots != NULL && i < f->roots->len; i++) {
		const AxisRootInfo *root = g_ptr_array_index(f->roots, i);
		const AxisVolumeInfo *volume = volume_of(f, root->volume_id);
		GString *into;

		if ((root->media_type == AXIS_MEDIA_MOVIES && !f->films) || (root->media_type == AXIS_MEDIA_TV && !f->series) ||
		    (root->media_type != AXIS_MEDIA_MOVIES && root->media_type != AXIS_MEDIA_TV))
			continue;
		into = root->volume_id == volume_id ? same : other;
		if (into->len > 0)
			g_string_append(into, ", ");
		if (into == other)
			g_string_append_printf(into, "%s (on %s)", root->name, volume != NULL ? volume->name : "another disk");
		else
			g_string_append(into, root->name);
	}
	if (same->len == 0 && other->len == 0)
		return g_strdup("No library root is set up to compare it with yet.");
	if (other->len == 0)
		return g_strdup_printf("Same disk as %s: imports from here are renames (or hardlinks), not copies.", same->str);
	if (same->len == 0)
		return g_strdup_printf("Not the disk of %s: every import from here is a copy.", other->str);
	return g_strdup_printf("Same disk as %s (renames or hardlinks); not the disk of %s (copies).", same->str,
		other->str);
}

static HtmxNode *
folders_table(
	const AxisSetupFigures       *f,
	const AxisDownloadClientInfo *client
){
	static const AxisWebColumn columns[] = {
		{ "Folder in the download client", AXIS_WEB_COL_NAME },
		{ "Same folder for Axis", AXIS_WEB_COL_WIDE },
		{ "Same disk as the library?", AXIS_WEB_COL_WIDE },
		{ NULL, 0 }
	};
	HtmxNode *table = axis_web_table_new("setup-folders", columns);
	HtmxTbody *body = htmx_tbody_new();
	guint n = 0;
	guint i;

	for (i = 0; i < f->folders->len; i++) {
		const AxisSetupFolder *folder = g_ptr_array_index(f->folders, i);
		HtmxTr *row;
		HtmxTd *path;

		if (folder->client_id != client->id)
			continue;
		n++;
		row = htmx_tr_new();
		path = htmx_td_new();
		ADD(path, htmx_code_new_with_text(folder->path));
		if (folder->reported)
			ADD(path, span_text("range-detail", "where its downloads were"));
		ADD(row, path);
		if (folder->mapped) {
			const AxisVolumeInfo *volume = volume_of(f, folder->volume_id);
			g_autofree gchar *where = volume == NULL ? NULL : folder->rel_path != NULL && folder->rel_path[0] != '\0' ?
				g_strdup_printf("%s/%s", volume->path, folder->rel_path) : g_strdup(volume->path);
			g_autofree gchar *on = volume != NULL ? g_strdup_printf(", on %s", volume->name) : NULL;
			g_autofree gchar *verdict = disk_verdict(f, folder->volume_id);
			HtmxTd *cell = htmx_td_new();

			/* A path wraps at its separators only. */
			if (where != NULL) {
				ADD(cell, axis_web_breakable(where));
				ADD(cell, span_text(NULL, on));
			} else {
				htmx_node_set_text_content(HTMX_NODE(cell), "a disk Axis no longer knows");
			}
			ADD(row, cell);
			ADD(row, htmx_td_new_with_text(verdict));
		} else {
			g_autofree gchar *text = g_strdup_printf("It %s.", folder->problem);
			HtmxTd *cell = htmx_td_new();

			ADD(cell, axis_web_setup_text("field-error", text));
			ADD(row, cell);
			ADD(row, htmx_td_new_with_text(""));
		}
		ADD(body, row);
	}
	ADD(table, body);
	if (n == 0) {
		g_object_unref(table);
		return NULL;
	}
	return table;
}

static void
step_clients(
	AxisWebSetup     *self,
	HtmxRequest      *request,
	AxisSetupFigures *f,
	HtmxNode         *body
){
	const AxisSetupState *s = &f->states[AXIS_SETUP_CLIENTS];
	const gchar *back = axis_setup_step_path(AXIS_SETUP_CLIENTS);
	HtmxNode *card = state_card(s, AXIS_SETUP_CLIENTS);
	guint n = f->clients != NULL ? f->clients->len : 0;
	HtmxNode *warning;
	HtmxNode *how;
	guint i;

	for (i = 0; i < n; i++) {
		const AxisDownloadClientInfo *client = g_ptr_array_index(f->clients, i);
		const AxisDownloadKindInfo *kind = axis_downloads_find_kind(self->downloads, client->kind);
		g_autofree gchar *text = axis_web_setup_client_sentence(client, kind);
		g_autofree gchar *href = g_strdup_printf("/downloads/clients/%" G_GINT64_FORMAT, client->id);
		gboolean good = client->enabled && client->state == AXIS_DOWNLOAD_CLIENT_OK && client->category_exists != 0;
		HtmxNode *what = NULL;
		HtmxDiv *acts = htmx_div_new();
		g_autofree gchar *link_words = g_strdup_printf("Download client page for %s", client->name);
		HtmxNode *edit = axis_web_setup_more("Change this download client's address, key or category");

		if (n == 1)
			what = solo_block(card);
		else
			ADD(card, item_line(good, good ? "Answered" : !client->enabled ? "Off" :
				client->state == AXIS_DOWNLOAD_CLIENT_UNCHECKED ? "Not tested" : "Problem", text, &what));
		htmx_element_add_class(HTMX_ELEMENT(acts), "actions");
		ADD(acts, axis_web_downloads_test_form(request, client->id, "Test this download client now", "button-small", back));
		ADD(acts, link_to(href, link_words));
		ADD(what, acts);
		if (client->state == AXIS_DOWNLOAD_CLIENT_REFUSED || !client->secret_set)
			htmx_element_set_attribute(HTMX_ELEMENT(edit), "open", "open");
		ADD(edit, axis_web_downloads_edit_form(self->downloads, request, client, back));
		ADD(what, edit);
	}
	ADD(body, card);
	if (s->off) {
		ADD(body, line_with_links("help", "Switch Downloads on in ", "/admin/modules", "Modules", " first.", NULL));
		return;
	}

	/* Where each client's files appear, and whether that is the library's disk. */
	if (f->clients != NULL && f->clients->len > 0) {
		card = card_new("Where the download client's files appear");
		ADD(card, axis_web_setup_text(NULL, "A client names its files by its own paths, its container's. Tell Axis "
			"which folder of the client's is where on its disks: once a mapping covers the folder finished downloads "
			"go to, Axis can find them. Here are the client's folders Axis knows of, where each is for Axis, and "
			"whether that is the disk of the library."));
		warning = axis_web_imports_volume_warnings(f->warnings);
		if (warning != NULL)
			ADD(card, warning);
		for (i = 0; i < f->clients->len; i++) {
			const AxisDownloadClientInfo *client = g_ptr_array_index(f->clients, i);
			HtmxNode *table = folders_table(f, client);
			HtmxHeading *name = htmx_h3_new();

			htmx_node_set_text_content(HTMX_NODE(name), client->name);
			ADD(card, name);
			if (table != NULL)
				ADD(card, table);
			else
				ADD(card, axis_web_setup_text("help", "No folder of its is mapped yet, and none of its downloads "
					"has finished to show one. Add the folder it puts finished downloads in."));
			if (f->volumes != NULL && f->volumes->len > 0) {
				HtmxNode *map = axis_web_downloads_map_form(request, client->id, f->volumes, back);

				ADD(card, axis_web_setup_text("guide-lead", "Add a path mapping"));
				/* Its fields wrap here as whole fields: the column is narrower
				 * than the client's own page. */
				htmx_element_add_class(HTMX_ELEMENT(map), "guide-row");
				ADD(card, map);
			} else
				ADD(card, line_with_links("help", "Add a disk first, in ", axis_setup_step_path(AXIS_SETUP_LIBRARY),
					"Disks and library folders", ": a path mapping points at one.", NULL));
		}
		ADD(card, axis_web_setup_guide_mapping());
		ADD(card, axis_web_setup_guide_one_disk());
		ADD(body, card);
	}

	card = card_new("The download client's address and key");
	how = how_list();
	how_add_text(how, "The address: the one you open the client at, as Axis can reach it, such as http://sabnzbd:8080 "
		"http://deluge:8112 or http://qbittorrent:8080 between containers on one network. A base path only if it is "
		"served under one.");
	how_add_text(how, "SABnzbd's key: in SABnzbd, open Config, then General. The API Key is under Security (not the "
		"NZB Key).");
	how_add_text(how, "Deluge's password: the one you log in to its web interface with (Preferences, Interface). Its "
		"Label plugin must be on, under Preferences, Plugins.");
	how_add_text(how, "qBittorrent's user name and password: the ones you log in to its web interface with (Tools, "
		"Options, Web UI). Its user name is admin until you change it. Type the password carefully: a few wrong "
		"logins make qBittorrent ban Axis's address for an hour, and Axis then waits instead of trying again.");
	how_add_text(how, "The category, or label in Deluge: leave axis, Axis's own, unless you have a reason.");
	ADD(card, how);
	for (i = 0; f->clients != NULL && i < f->clients->len; i++) {
		const AxisDownloadClientInfo *client = g_ptr_array_index(f->clients, i);
		gboolean sab = strcmp(client->kind, AXIS_DOWNLOAD_SABNZBD_NAME) == 0;
		g_autofree gchar *words = g_strdup_printf(sab ? "Open %s's General settings" : "Open %s", client->name);
		HtmxNode *link = axis_web_setup_link_to_service(client->address, client->base_path, sab ? "/config/general/" :
			"/", words);

		if (link != NULL && (sab || strcmp(client->kind, "deluge") == 0 || strcmp(client->kind, "qbittorrent") == 0)) {
			HtmxP *p = htmx_p_new();

			htmx_element_add_class(HTMX_ELEMENT(p), "guide-service-link");
			ADD(p, link);
			ADD(card, p);
		} else if (link != NULL) {
			g_object_unref(link);
		}
	}
	ADD(card, line_with_links("help", "More in ", SABNZBD_DOCS, "SABnzbd's documentation", ", ", DELUGE_DOCS,
		"Deluge's", " and ", QBITTORRENT_DOCS, "qBittorrent's", ".", NULL));
	if (f->clients == NULL || f->clients->len == 0) {
		ADD(card, axis_web_downloads_add_form(self->downloads, request, back));
	} else {
		HtmxNode *more = axis_web_setup_more("Add another download client");

		ADD(more, axis_web_downloads_add_form(self->downloads, request, back));
		ADD(card, more);
	}
	ADD(card, axis_web_setup_guide_category());
	ADD(card, axis_web_setup_text("help", AXIS_IMPORT_TORRENTS_HELP));
	ADD(body, card);
}

/* ── Step 5: what to download ───────────────────────────────────── */

static void
step_quality(
	AxisWebSetup     *self,
	HtmxRequest      *request,
	AxisSetupFigures *f,
	HtmxNode         *body
){
	static const gchar *const keys[] = { AXIS_QUALITY_SETTING_DEFAULT, NULL };
	const AxisSetupState *s = &f->states[AXIS_SETUP_QUALITY];
	g_autoptr(GPtrArray) details = g_ptr_array_new_with_free_func(g_free);
	g_autofree AxisWebSettingsChoice *choices = NULL;
	HtmxNode *card = state_card(s, AXIS_SETUP_QUALITY);
	HtmxNode *form;
	guint n = 0;
	guint i;

	ADD(body, card);
	if (s->off && f->profiles == NULL) {
		ADD(body, line_with_links("help", "Switch Quality profiles on in ", "/admin/modules", "Modules", " first.", NULL));
		return;
	}
	card = card_new("Default quality profile for new movies and series");
	ADD(card, axis_web_setup_text(NULL, "Each profile ranks the qualities it accepts and the sizes it allows. Pick the "
		"one most titles should follow; a title can have another, chosen when it is added or later on its page."));
	choices = g_new0(AxisWebSettingsChoice, f->profiles->len + 1);
	for (i = 0; i < f->profiles->len; i++) {
		const AxisQualityStored *stored = g_ptr_array_index(f->profiles, i);
		const AxisQualityBuiltin *builtin = axis_quality_builtin(stored->slug);
		gchar *detail;

		if (!stored->usable)
			continue;
		detail = builtin != NULL ? g_strdup(builtin->summary) : stored->copied_from != NULL ?
			g_strdup_printf("Your own, copied from %s.", stored->copied_from) : g_strdup("Your own.");
		/* Which one is the default is the choice made here, not the
		 * built-in's own line. */
		if (g_str_has_suffix(detail, " The default for a new title."))
			detail[strlen(detail) - strlen(" The default for a new title.")] = '\0';
		g_ptr_array_add(details, detail);
		choices[n].value = stored->slug;
		choices[n].label = stored->name;
		choices[n].detail = detail;
		n++;
	}
	form = settings_form(self, request, AXIS_QUALITY_MODULE_ID, keys, "Make this the default quality profile", AXIS_SETUP_QUALITY,
		AXIS_QUALITY_SETTING_DEFAULT, choices, n);
	if (form != NULL)
		ADD(card, form);
	ADD(card, line_with_links("help", "To see what a profile makes of a release name, ", "/quality/try", "try a name",
		". To change one, copy it on the ", "/quality", "Quality profiles", " page: the built-ins stay as they are.",
		NULL));
	ADD(body, card);
}

/* ── Step 6: players ────────────────────────────────────────────── */

static HtmxNode *
server_check_form(
	HtmxRequest *request,
	const gchar *kind,
	const gchar *label
){
	HtmxNode *form = axis_web_auth_post_form(request, AXIS_WEB_SETUP_PATH "/players/check");
	g_autofree gchar *words = g_strdup_printf("Test the %s connection", label);

	htmx_element_add_class(HTMX_ELEMENT(form), "inline-form");
	axis_web_auth_add(form, axis_web_auth_hidden("server", kind));
	axis_web_auth_add(form, axis_web_auth_button(words, "button-small"));
	return form;
}

static void
step_players(
	AxisWebSetup     *self,
	HtmxRequest      *request,
	AxisSetupFigures *f,
	HtmxNode         *body
){
	static const gchar *const plex_keys[] = {
		AXIS_COMPAT_SETTING_ADDRESS, AXIS_COMPAT_SETTING_TOKEN, AXIS_COMPAT_SETTING_UNTRUSTED_TLS, NULL
	};
	static const gchar *const jellyfin_keys[] = {
		AXIS_COMPAT_JELLYFIN_ADDRESS, AXIS_COMPAT_JELLYFIN_TOKEN, AXIS_COMPAT_JELLYFIN_UNTRUSTED_TLS, NULL
	};
	const AxisSetupState *s = &f->states[AXIS_SETUP_PLAYERS];
	HtmxNode *card = state_card(s, AXIS_SETUP_PLAYERS);
	HtmxNode *answer = check_notice(f);
	HtmxNode *how;
	HtmxNode *form;
	guint i;

	if (answer != NULL)
		ADD(card, answer);
	for (i = 0; f->coverage_read && f->coverage.servers != NULL && i < f->coverage.servers->len; i++) {
		const AxisCompatServerStatus *server = g_ptr_array_index(f->coverage.servers, i);
		g_autofree gchar *when = axis_web_format_time(server->last_ok_us, "never");
		g_autofree gchar *text = NULL;
		gboolean good = server->last_ok_us > 0 && server->failures == 0;
		HtmxNode *what = NULL;
		HtmxNode *line;

		if (!server->configured)
			continue;
		text = good ? g_strdup_printf("%s: the last look that worked was at %s.", server->label, when) :
			server->last_error != NULL ? g_strdup_printf("%s: the last look failed: %s", server->label,
			server->last_error) : g_strdup_printf("%s is set up and has not been reached yet.", server->label);
		line = item_line(good, good ? "Answered" : "Not yet", text, &what);
		ADD(what, server_check_form(request, server->kind, server->label));
		ADD(card, line);
	}
	ADD(body, card);
	if (s->off) {
		if (self->compat != NULL)
			ADD(body, line_with_links("help", "Switch Player compatibility on in ", "/admin/modules", "Modules",
				" to set a media server up.", NULL));
		return;
	}

	card = card_new("Plex");
	how = how_list();
	how_add_text(how, "The address: the one Axis can reach the server at, with its port, such as "
		"http://192.168.1.10:32400.");
	how_add(how, line_with_links(NULL, "The token: sign in to Plex in a browser, open any movie's menu, choose Get "
		"Info, then View XML. The address of the page that opens ends in X-Plex-Token= and the token. ",
		PLEX_TOKEN_HELP, "Plex's help on finding the token", ".", NULL));
	ADD(card, how);
	form = settings_form(self, request, AXIS_COMPAT_MODULE_ID, plex_keys, "Save the Plex connection", AXIS_SETUP_PLAYERS, NULL,
		NULL, 0);
	if (form != NULL)
		ADD(card, form);
	ADD(body, card);

	card = card_new("Jellyfin");
	how = how_list();
	how_add_text(how, "The address: the one Axis can reach the server at, with its port, such as "
		"http://192.168.1.11:8096.");
	how_add(how, line_with_links(NULL, "The token: in Jellyfin, open the Dashboard, then API Keys, and add one named "
		"Axis. ", JELLYFIN_DOCS, "Jellyfin's documentation", ".", NULL));
	ADD(card, how);
	form = settings_form(self, request, AXIS_COMPAT_MODULE_ID, jellyfin_keys, "Save the Jellyfin connection", AXIS_SETUP_PLAYERS,
		NULL, NULL, 0);
	if (form != NULL)
		ADD(card, form);
	ADD(card, axis_web_setup_text("help", "Axis reads from these servers, and after an import asks them to rescan the one "
		"folder that changed. It changes nothing else on them. Either, or both, will do."));
	ADD(body, card);
}

/* ── Step 7: letting Axis write ─────────────────────────────────── */

static void
step_writing(
	AxisWebSetup     *self,
	HtmxRequest      *request,
	AxisSetupFigures *f,
	HtmxNode         *body
){
	static const gchar *const keys[] = { AXIS_STORAGE_SETTING_READ_ONLY, NULL };
	const AxisSetupState *s = &f->states[AXIS_SETUP_WRITING];
	HtmxNode *card = state_card(s, AXIS_SETUP_WRITING);
	HtmxNode *form;

	ADD(body, card);
	card = card_new("What changes when Axis may write");
	ADD(card, axis_web_setup_guide_writing(f->recycle_s));
	ADD(body, card);

	card = card_new("Read-only storage");
	ADD(card, axis_web_setup_text(NULL, f->read_only ? "This is the setting Read-only storage, as Settings has it "
		"under Storage. Choose Off and save it to let Axis write; you can switch it back on there at any time, and "
		"from then on nothing more is written (what was imported stays)." : "This is the setting Read-only storage, "
		"as Settings has it under Storage. It is off: Axis may write. Switch it back on to stop all writing; what "
		"was imported stays."));
	form = settings_form(self, request, AXIS_STORAGE_MODULE_ID, keys, "Save", AXIS_SETUP_WRITING, NULL, NULL, 0);
	if (form != NULL)
		ADD(card, form);
	ADD(body, card);
}

/* ── Step 8: recognising what is already there ──────────────────── */

static void
step_existing(
	AxisWebSetup     *self,
	HtmxRequest      *request,
	AxisSetupFigures *f,
	HtmxNode         *body
){
	const AxisSetupState *s = &f->states[AXIS_SETUP_EXISTING];
	const gchar *back = axis_setup_step_path(AXIS_SETUP_EXISTING);
	HtmxNode *card = state_card(s, AXIS_SETUP_EXISTING);
	guint n = f->identify_roots != NULL ? f->identify_roots->len : 0;
	guint i;

	for (i = 0; i < n; i++) {
		const AxisIdentifyRootInfo *root = g_ptr_array_index(f->identify_roots, i);
		g_autofree gchar *href = g_strdup_printf("/identify/roots/%" G_GINT64_FORMAT, root->root_id);
		g_autofree gchar *link_words = g_strdup_printf("Identify page for %s", root->name);
		g_autofree gchar *when = axis_web_format_time(root->last_run_us, "");
		g_autofree gchar *text = root->last_run_us > 0 ? g_strdup_printf("%s, last looked at %s: %u recognised, "
			"%u likely (to confirm), %u unknown.", root->name, when, root->sure + root->confirmed, root->likely,
			root->unknown) : g_strdup_printf("%s has not been looked at yet%s.", root->name, root->running ?
			"; a run is in progress" : "");
		HtmxNode *what = NULL;
		HtmxDiv *acts = htmx_div_new();

		if (n == 1)
			what = solo_block(card);
		else
			ADD(card, item_line(root->last_run_us > 0, root->last_run_us > 0 ? "Looked at" : "Not yet", text, &what));
		htmx_element_add_class(HTMX_ELEMENT(acts), "actions");
		ADD(acts, axis_web_identify_run_form(request, root->root_id, back));
		ADD(acts, link_to(href, root->likely + root->unknown > 0 ? "Review what waits for you" : link_words));
		ADD(what, acts);
	}
	ADD(body, card);
	if (s->off) {
		if (self->identify != NULL)
			ADD(body, line_with_links("help", "Switch Identification on in ", "/admin/modules", "Modules", " first.",
				NULL));
		return;
	}
	card = card_new("How Axis identifies your movies");
	ADD(card, axis_web_setup_text(NULL, "Axis reads the names of the folders and files in each library folder for "
		"Movies, and the identifiers they carry, asks TMDb which movie each one is, and links what it is sure of. "
		"A likely match waits for you to confirm it, an unknown one for you to search; nothing is guessed, and "
		"nothing on disk is changed, moved or renamed."));
	ADD(card, axis_web_setup_text("help", "Once a movie is linked, Axis knows you have it: it is not searched for "
		"again, and a release is weighed against the file you have. It runs by itself within ten minutes of a scan; "
		"Identify new and changed files now starts it at once. Series on disk are recognised the same way, under TV."));
	ADD(body, card);
}

/* ── The pages ──────────────────────────────────────────────────── */

static const gchar *
step_intro(
	const AxisSetupFigures *f,
	AxisSetupStep           step
){
	gboolean tmdb = FALSE;
	guint i;

	for (i = 0; f->providers != NULL && i < f->providers->len; i++)
		tmdb = tmdb || strcmp(g_array_index(f->providers, AxisMetaProviderStatus, i).info->name,
			AXIS_META_TMDB_NAME) == 0;
	switch (step) {
	case AXIS_SETUP_TITLES:
		return tmdb ? "Where Axis learns which movies and series exist: their names, years, posters, overviews and "
			"episodes come from TMDb, The Movie Database. Searching, adding a title and recognising what is already "
			"on disk all ask it, with a token of your own." : "Where Axis learns which movies and series exist: their "
			"names, years, posters, overviews and episodes come from a metadata provider. Searching, adding a title "
			"and recognising what is already on disk all ask it, with a key of your own.";
	case AXIS_SETUP_LIBRARY:
		return f->films && !f->series ? "The disks Axis may use, and the folders on them that hold your movies. Axis "
			"scans these folders to know what you have, and imports what it downloads into them." : !f->films &&
			f->series ? "The disks Axis may use, and the folders on them that hold your series. Axis scans these "
			"folders to know what you have, and imports what it downloads into them." : "The disks Axis may use, and "
			"the folders on them that hold your movies and series. Axis scans these folders to know what you have, and "
			"imports what it downloads into them.";
	case AXIS_SETUP_INDEXERS:
		return "Where Axis looks for releases to download: your Usenet indexers, which Axis asks itself with the API "
			"key each gives you, and Prowlarr if you use it, which searches every indexer and tracker configured in it "
			"with one request. One of the two is enough; with both, an indexer in each is asked once, directly.";
	case AXIS_SETUP_CLIENTS:
		return "The programs that do the downloading: SABnzbd for Usenet, Deluge or qBittorrent for torrents. Axis "
			"hands them what it chose, follows what it added and takes the files when they finish, so it needs each "
			"one's address and key, and where its files appear on Axis's disks.";
	case AXIS_SETUP_QUALITY:
		return "Which releases of a title Axis takes: a quality profile lists the qualities it accepts, best first, "
			"the sizes it allows, and when a better release is worth fetching over the file you have. A title gets "
			"the default profile when it is added.";
	case AXIS_SETUP_PLAYERS:
		return "Plex or Jellyfin, if you use one. Axis reads, and never changes, which devices play what and whether "
			"they play it directly, so that a conversion never leaves a file your players cannot play. Finding and "
			"downloading do not need it.";
	case AXIS_SETUP_WRITING:
		return "Until you allow it, Axis changes nothing on your disks: downloads arrive at the client, but nothing "
			"is imported into the library. Here is what changes when you allow it, and what never does.";
	default:
		return "The movies already on your disks, matched to titles by their folder and file names, so that Axis knows "
			"what you have: it does not search for them again, and an upgrade is weighed against the file you have. "
			"Nothing on disk changes, and an uncertain match waits for you.";
	}
}

HtmxResponse *
axis_web_setup_step_page(
	AxisWebSetup     *self,
	HtmxRequest      *request,
	AxisSetupFigures *f,
	AxisSetupStep     step,
	const gchar      *done
){
	g_autoptr(HtmxNode) page = NULL;
	HtmxDiv *body = htmx_div_new();
	HtmxHeading *title = htmx_h1_new();
	g_autofree gchar *kicker = g_strdup_printf("Step %u of %u%s", (guint)step + 1, (guint)AXIS_SETUP_N_STEPS,
		axis_setup_step_optional(step) ? ", optional" : "");
	g_autofree gchar *page_title = g_strdup_printf("%s - Setup guide", axis_setup_step_label(step));
	const gchar *message = done_text(step, done);
	AxisWebNotice notice;

	ADD(body, axis_web_setup_text("guide-kicker", kicker));
	htmx_node_set_text_content(HTMX_NODE(title), axis_setup_step_label(step));
	ADD(body, title);
	ADD(body, axis_web_page_intro(step_intro(f, step)));
	axis_web_notice_begin(&notice, request, FALSE, g_strdup(message), HTMX_NODE(body));
	switch (step) {
	case AXIS_SETUP_TITLES:
		step_titles(self, request, f, HTMX_NODE(body));
		break;
	case AXIS_SETUP_LIBRARY:
		step_library(self, request, f, HTMX_NODE(body));
		break;
	case AXIS_SETUP_INDEXERS:
		step_indexers(self, request, f, HTMX_NODE(body));
		break;
	case AXIS_SETUP_CLIENTS:
		step_clients(self, request, f, HTMX_NODE(body));
		break;
	case AXIS_SETUP_QUALITY:
		step_quality(self, request, f, HTMX_NODE(body));
		break;
	case AXIS_SETUP_PLAYERS:
		step_players(self, request, f, HTMX_NODE(body));
		break;
	case AXIS_SETUP_WRITING:
		step_writing(self, request, f, HTMX_NODE(body));
		break;
	default:
		step_existing(self, request, f, HTMX_NODE(body));
		break;
	}
	ADD(body, step_actions(request, &f->states[step], step, &notice));
	axis_web_notice_end(&notice);
	page = frame(f, (gint)step, HTMX_NODE(body));
	return axis_web_page_respond(request, page_title, page);
}

HtmxResponse *
axis_web_setup_overview_page(
	AxisWebSetup     *self,
	HtmxRequest      *request,
	AxisSetupFigures *f,
	const gchar      *done
){
	g_autoptr(HtmxNode) page = NULL;
	HtmxDiv *body = htmx_div_new();
	HtmxHeading *title = htmx_h1_new();
	HtmxNode *card;
	HtmxOl *list = htmx_ol_new();
	const gchar *message = done_text(-1, done);
	gint resume = axis_setup_figures_resume(f);
	guint needed = 0;
	guint needed_done = 0;
	guint optional = 0;
	guint optional_done = 0;
	guint i;

	htmx_node_set_text_content(HTMX_NODE(title), "Setup guide");
	ADD(body, title);
	ADD(body, axis_web_page_intro("Everything Axis needs before it can find, download and import a movie or a series, "
		"one step at a time: what each thing is, where its key or address is found, and a test that says whether it "
		"works. You do not have to use this guide: every setting is also on its own page, and you can leave at any "
		"step and come back."));
	if (message != NULL)
		ADD(body, axis_web_auth_notice(FALSE, message));

	for (i = 0; i < AXIS_SETUP_N_STEPS; i++) {
		if (axis_setup_step_optional(i)) {
			optional++;
			optional_done += f->states[i].done;
		} else {
			needed++;
			needed_done += f->states[i].done;
		}
	}
	card = card_new("Setup progress");
	htmx_element_add_class(HTMX_ELEMENT(card), "guide-summary");
	{
		g_autofree gchar *rest = optional_done == 0 ? g_strdup_printf("The other %u steps are optional.", optional) :
			g_strdup_printf("Of the %u optional steps, %u %s done.", optional, optional_done, optional_done == 1 ?
			"is" : "are");
		g_autofree gchar *text = needed_done == needed ? g_strdup_printf("Everything Axis needs is set up: a movie or a "
			"series can be found, downloaded and imported. %s", rest) : g_strdup_printf("%u of the %u steps Axis "
			"needs %s done. %s", needed_done, needed, needed_done == 1 ? "is" : "are", rest);

		ADD(card, axis_web_setup_text("guide-summary-text", text));
	}
	if (resume >= 0) {
		g_autofree gchar *words = g_strdup_printf("%s with step %d: %s", resume == 0 ? "Start" : "Carry on",
			resume + 1, axis_setup_step_label((AxisSetupStep)resume));
		HtmxNode *go = link_to(axis_setup_step_path((AxisSetupStep)resume), words);

		htmx_element_add_class(HTMX_ELEMENT(go), "guide-continue");
		ADD(card, go);
	}
	ADD(body, card);

	htmx_element_add_class(HTMX_ELEMENT(list), "guide-overview");
	for (i = 0; i < AXIS_SETUP_N_STEPS; i++) {
		const AxisSetupState *s = &f->states[i];
		HtmxLi *item = htmx_li_new();
		HtmxDiv *head = htmx_div_new();
		HtmxA *name = htmx_a_new_with_href(axis_setup_step_path(i));
		g_autofree gchar *number = g_strdup_printf("%u", i + 1);
		g_autofree gchar *css = g_strdup_printf("guide-step-%s", state_class(s, i));

		htmx_element_add_class(HTMX_ELEMENT(item), "guide-overview-step");
		htmx_element_add_class(HTMX_ELEMENT(item), css);
		htmx_element_add_class(HTMX_ELEMENT(head), "guide-overview-head");
		ADD(head, span_text("guide-step-number", number));
		htmx_node_set_text_content(HTMX_NODE(name), axis_setup_step_label(i));
		htmx_element_add_class(HTMX_ELEMENT(name), "guide-overview-name");
		ADD(head, name);
		ADD(head, state_badge(s, i));
		ADD(item, head);
		ADD(item, axis_web_setup_text("guide-overview-text", s->summary != NULL ? s->summary : ""));
		ADD(list, item);
	}
	ADD(body, list);
	page = frame(f, -1, HTMX_NODE(body));
	return axis_web_page_respond(request, "Setup guide", page);
}
