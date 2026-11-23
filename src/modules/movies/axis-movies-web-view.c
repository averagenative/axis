/*
 * axis-movies-web-view.c - the movies module's pieces and pages: the Films
 * grid, Wanted, History, and on a film's title page its state, its
 * administrators' buttons, the add form, and its cards (the download, the
 * releases of its last search with every verdict and reason, its blocklist,
 * its options, its history)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 *   GET /films            the movies, managed and on disk alike, as posters,
 *                         each with its state's mark (?state=<name> filters,
 *                         ?sort=added|title|year|size), and what
 *                         identification holds back for a person
 *   GET /films/wanted     the films wanted or needing attention, with why
 *                         (?state=attention for those alone)
 *   GET /films/history    what happened to every film, the latest first
 *                         (?before=<event id> for the next page)
 *
 * Everything that came from a provider, an indexer or a client (titles,
 * release names, messages) is set as text; nothing here writes markup by
 * hand.  Every form posts with the session's CSRF field and is drawn for
 * administrators only.
 */

#include "modules/movies/axis-movies-web.h"

#include <string.h>

#include "core/axis-size.h"
#include "web/axis-web-activity.h"
#include "web/axis-web-auth-private.h"
#include "web/axis-web-back.h"
#include "web/axis-web-browser-private.h"
#include "web/axis-web-downloads-private.h"
#include "web/axis-web-manual.h"
#include "web/axis-web-meta-private.h"
#include "web/axis-web-place.h"
#include "web/axis-web-private.h"
#include "web/axis-web-storage-private.h"
#include "web/axis-web-titles.h"

#define ADD axis_web_storage_add

/* The foot of the list: what it holds, and what Axis does with what it did
 * not add. */
static const gchar list_note[] = "This list is the whole movie library: the movies added to Axis, and every movie "
	"on disk that identification (Library, Identify) has recognised. A movie on disk that Axis does not "
	"manage is never searched for, downloaded, upgraded, renamed or moved; open it and choose Manage this movie to "
	"change that. Removing a movie from Axis never deletes its files.";

/* The foot of a managed movie's page and of the add form. */
static const gchar library_note[] = "A movie whose file identification (Library, Identify) recognises on "
	"disk is in the library and is not searched for, whether it was added before or after.";

/* ── Small pieces ───────────────────────────────────────────────── */

static HtmxNode *
heading(
	guint        level,
	const gchar *text
){
	HtmxHeading *h = level == 1 ? htmx_h1_new() : level == 2 ? htmx_h2_new() : htmx_h3_new();

	htmx_node_set_text_content(HTMX_NODE(h), text);
	return HTMX_NODE(h);
}

static HtmxNode *
cell_text(const gchar *text)
{
	return HTMX_NODE(htmx_td_new_with_text(text != NULL ? text : ""));
}

static HtmxNode *
cell_node(HtmxNode *node)
{
	HtmxTd *cell = htmx_td_new();

	if (node != NULL)
		ADD(cell, node);
	return HTMX_NODE(cell);
}

static HtmxNode *
cell_time(gint64 us)
{
	g_autofree gchar *text = axis_web_format_time(us, "");

	return cell_text(text);
}

/* Text that may hold a release name or a path: it wraps after its dots,
 * hyphens, underscores and slashes. */
static HtmxNode *
cell_breakable(const gchar *text)
{
	return cell_node(axis_web_breakable(text != NULL ? text : ""));
}

static HtmxNode *
name_cell(const gchar *text)
{
	HtmxBdi *bdi = htmx_bdi_new();

	ADD(bdi, axis_web_breakable(text != NULL ? text : ""));
	return cell_node(HTMX_NODE(bdi));
}

/* A one-button form posting to @action, with hidden @name=@value (nullable). */
static HtmxNode *
button_form(
	HtmxRequest *request,
	const gchar *action,
	const gchar *name,
	const gchar *value,
	const gchar *label,
	const gchar *css
){
	HtmxNode *form = axis_web_auth_post_form(request, action);

	htmx_element_add_class(HTMX_ELEMENT(form), "film-button");
	if (name != NULL)
		ADD(form, axis_web_auth_hidden(name, value));
	ADD(form, axis_web_auth_button(label, css));
	return form;
}

gchar *
axis_movies_web_age(gint64 us)
{
	gint64 seconds = (g_get_real_time() - us) / G_USEC_PER_SEC;

	if (us <= 0)
		return g_strdup("");
	if (seconds < 3600)
		return g_strdup("less than an hour");
	if (seconds < 48 * 3600)
		return g_strdup_printf("%" G_GINT64_FORMAT " hours", seconds / 3600);
	return g_strdup_printf("%" G_GINT64_FORMAT " days", seconds / 86400);
}

const gchar *
axis_movies_web_done_message(const gchar *key)
{
	static const struct
	{
		const gchar *key;
		const gchar *text;
	} messages[] = {
		{ "added", "The movie was added." },
		{ "searching", "A search is queued: this page shows what it found once it has run." },
		{ "already_searching", "Nothing new was queued: a search of it was queued or running already." },
		{ "searched_recently", "Nothing new was queued: it was searched for a moment ago. You can ask again in a "
		  "minute." },
		{ "grabbed", "The release was sent to a download client." },
		{ "given_up", "Axis gave up on the download: it is on the blocklist, it is being removed from its client, "
		  "and the next best release is sought. A torrent's client deletes its files; SABnzbd keeps the files of a job "
		  "that finished, and Axis deletes them itself where the rules allow it. The Downloads page says what became "
		  "of them." },
		{ "cleared", "Taken off the blocklist." },
		{ "kept", "The file is kept by hand: no automatic search and no Search now replaces it. A release you grab "
		  "yourself from a manual search still can." },
		{ "let_go", "The file is no longer kept by hand: Axis upgrades it by the ordinary rules again." },
		{ "started", "Axis searches for this movie by itself from now on, at the scheduler's ordinary pace. Nothing "
		  "was searched for just now." },
		{ "not_held", "This movie was not held: Axis searches for it by itself already." },
		{ "saved", "The movie's settings were saved." },
		{ "removed", "The movie was removed. No file was touched." },
		{ "managed", "Axis manages this movie now. Its file was on disk already, so nothing was searched for, and "
		  "nothing on disk was touched." },
		{ "unmanaged", "Axis no longer manages this movie. No file was touched: its file is still on disk, so it "
		  "stays in Movies as In library." },
		{ NULL, NULL }
	};
	guint i;

	for (i = 0; key != NULL && messages[i].key != NULL; i++) {
		if (strcmp(messages[i].key, key) == 0)
			return messages[i].text;
	}
	return NULL;
}

gchar *
axis_movies_web_done_text(
	const gchar          *key,
	const AxisMovieFacts *facts
){
	const gchar *name = facts != NULL && facts->profile_name != NULL ? facts->profile_name : "the new one";

	if (g_strcmp0(key, "profile") == 0)
		return g_strdup_printf("The quality profile is %s now, and the movie is searched for again with it: this page "
			"shows what the search found once it has run.", name);
	if (g_strcmp0(key, "profile_unmonitored") == 0)
		return g_strdup_printf("The quality profile is %s now. The movie is not monitored, so nothing was searched "
			"for: use Search for this movie to search for it with the new profile.", name);
	if (g_strcmp0(key, "profile_none") == 0)
		return g_strdup_printf("The quality profile is %s now. Nothing was searched for: the movie is not wanted under "
			"it now (its file is at the cutoff, it is not out yet, or a search or a download of it is under way).",
			name);
	return g_strdup(axis_movies_web_done_message(key));
}

/* ── A film's state ─────────────────────────────────────────────── */

HtmxNode *
axis_movies_web_mark(AxisMovieState state)
{
	HtmxSpan *mark = htmx_span_new();
	g_autofree gchar *css = g_strdup_printf("film-mark-%s", axis_movie_state_name(state));

	htmx_element_add_class(HTMX_ELEMENT(mark), "film-mark");
	htmx_element_add_class(HTMX_ELEMENT(mark), css);
	htmx_element_set_attribute(HTMX_ELEMENT(mark), "title", axis_movie_state_label(state));
	htmx_node_set_text_content(HTMX_NODE(mark), axis_movie_state_short(state));
	return HTMX_NODE(mark);
}

/* The mark of a movie in a list or on its page: its state, or "Held" for
 * one migrated whose searches are not started and that would otherwise
 * read as being searched for (import-from-arr). */
static HtmxNode *
mark_of(const AxisMoviesShown *shown)
{
	const gchar *hold = shown->facts != NULL && !shown->facts->library_only ? shown->facts->search_hold : NULL;
	HtmxNode *mark = axis_movies_web_mark(shown->state);

	if (!axis_movie_hold_stops(hold) || (shown->state != AXIS_MOVIE_WANTED && shown->state != AXIS_MOVIE_UPGRADE_WANTED &&
	    shown->state != AXIS_MOVIE_ATTENTION && shown->state != AXIS_MOVIE_NOT_AVAILABLE))
		return mark;
	htmx_element_add_class(HTMX_ELEMENT(mark), "film-mark-held");
	htmx_element_set_attribute(HTMX_ELEMENT(mark), "title", axis_movie_hold_sentence(hold));
	htmx_node_set_text_content(mark, axis_movie_hold_short(hold));
	return mark;
}

HtmxNode *
axis_movies_web_state_block(const AxisMoviesShown *shown)
{
	const AxisMovieFacts *f = shown->facts;
	HtmxDiv *block = htmx_div_new();
	HtmxDiv *line = htmx_div_new();
	g_autofree gchar *id = f->library_only ? g_strdup_printf("film-state-title-%" G_GINT64_FORMAT, f->title_id) :
		g_strdup_printf("film-state-%" G_GINT64_FORMAT, f->id);
	g_autofree gchar *css = g_strdup_printf("film-state-%s", axis_movie_state_name(shown->state));

	htmx_element_set_id(HTMX_ELEMENT(block), id);
	htmx_element_add_class(HTMX_ELEMENT(block), "film-state");
	htmx_element_add_class(HTMX_ELEMENT(block), css);
	/* The block follows what happens to the film: when the hub says one of
	 * its searches or downloads changed, it is fetched again (htmx and its
	 * SSE extension; without them the page is complete as it is); and by
	 * time while something is in hand, and once when "a moment ago" ends. */
	if (!f->library_only) {
		g_autofree gchar *path = g_strdup_printf("/films/%" G_GINT64_FORMAT "/state", f->id);
		const AxisMoviesWork *work = shown->work;
		gboolean in_hand = axis_movie_in_flight(f) || (work != NULL && (work->search != NULL || work->list != NULL));
		gint64 wake = work == NULL ? 0 : axis_web_activity_sooner(axis_web_activity_wake(work->ended_us,
			work->now_us), axis_web_activity_wake(work->list_ended_us, work->now_us));
		g_autofree gchar *trigger = axis_web_activity_trigger(in_hand, wake);

		htmx_element_set_attribute(HTMX_ELEMENT(block), "hx-get", path);
		htmx_element_set_attribute(HTMX_ELEMENT(block), "hx-trigger", trigger);
		htmx_element_set_attribute(HTMX_ELEMENT(block), "hx-swap", "outerHTML");
	}
	htmx_element_add_class(HTMX_ELEMENT(line), "film-state-line");
	ADD(line, mark_of(shown));
	if (f->library_only) {
		ADD(line, axis_web_storage_text("film-state-profile", "On disk, not managed by Axis"));
	} else if (f->profile_name != NULL) {
		g_autofree gchar *profile = g_strdup_printf("Quality profile: %s, %s", f->profile_name, f->monitored ? "monitored" :
			"not monitored");

		ADD(line, axis_web_storage_text("film-state-profile", profile));
		ADD(line, axis_web_browser_link("#settings", "Settings for this movie"));
	}
	ADD(block, line);
	if (shown->reason != NULL)
		ADD(block, axis_web_storage_text("film-reason", shown->reason));
	/* What needs attention links to where it is put right, on this page. */
	switch (shown->kind) {
	case AXIS_ACQUIRE_ATTENTION_NONE:
	case AXIS_ACQUIRE_ATTENTION_IMPORT:
		/* (The blocked import's link follows.) */
		break;
	case AXIS_ACQUIRE_ATTENTION_LEFT_AT_CLIENT:
		ADD(block, axis_web_browser_link("/downloads", "See the download on the Downloads page"));
		break;
	case AXIS_ACQUIRE_ATTENTION_NO_ROOT:
	case AXIS_ACQUIRE_ATTENTION_PROFILE_UNUSABLE:
		/* (Settings for this movie, on the line above.) */
		break;
	case AXIS_ACQUIRE_ATTENTION_ROOT_OFFLINE:
		ADD(block, axis_web_browser_link("/storage", "See the disk on the Disks page"));
		break;
	default:
		ADD(block, axis_web_browser_link("#resolve", "See what was found and what you can do"));
		break;
	}
	if (shown->state == AXIS_MOVIE_DOWNLOADING && f->download_known && f->download_size > 0 && f->download_done >= 0)
		ADD(block, axis_web_downloads_bar((gdouble)f->download_done / (gdouble)f->download_size));
	/* What is happening now, or what its last search found lately. */
	if (!f->library_only)
		ADD(block, axis_movies_web_film_line(shown));
	/* Its import, when that is what it waits for or what needs a person. */
	if (f->import_id > 0 && (shown->state == AXIS_MOVIE_TO_IMPORT || shown->state == AXIS_MOVIE_ATTENTION) &&
	    g_strcmp0(f->import_state, "imported") != 0 && g_strcmp0(f->import_state, "dismissed") != 0) {
		g_autofree gchar *href = g_strdup_printf("/imports/%" G_GINT64_FORMAT, f->import_id);

		ADD(block, axis_web_browser_link(href, g_strcmp0(f->import_state, "blocked") == 0 ?
			"See the blocked import and what resolves it" : "See the import"));
	}
	return HTMX_NODE(block);
}

/* Where a film that needs attention is put right: its blocked import, the
 * Downloads page, else the box on its own page that says what to do. */
static gchar *
attention_href(const AxisMoviesShown *shown)
{
	const AxisMovieFacts *f = shown->facts;

	if (shown->kind == AXIS_ACQUIRE_ATTENTION_IMPORT && f->import_id > 0)
		return g_strdup_printf("/imports/%" G_GINT64_FORMAT, f->import_id);
	if (shown->kind == AXIS_ACQUIRE_ATTENTION_LEFT_AT_CLIENT)
		return g_strdup("/downloads");
	if (shown->kind == AXIS_ACQUIRE_ATTENTION_NO_ROOT || shown->kind == AXIS_ACQUIRE_ATTENTION_PROFILE_UNUSABLE)
		return g_strdup_printf("/title/%" G_GINT64_FORMAT "#settings", f->title_id);
	return g_strdup_printf("/title/%" G_GINT64_FORMAT "#resolve", f->title_id);
}

/* "2 copies of it are on disk ..." for a movie with more than one copy. */
static gchar *
copies_text(guint copies)
{
	if (copies <= 1)
		return NULL;
	return g_strdup_printf("%u copies of this movie are on disk. One is enough; Axis lists them all here and removes "
		"none of them.", copies);
}

/* The film's files in the library, each with how it came there. */
static HtmxNode *
library_card(
	HtmxRequest           *request,
	const AxisMovieDetail *detail,
	const AxisMovieFacts  *f,
	gboolean               admin,
	AxisWebNotice         *notice
){
	static const AxisWebColumn columns[] = {
		{ "File", AXIS_WEB_COL_NAME },
		{ "Library folder", AXIS_WEB_COL_TEXT },
		{ "Size", AXIS_WEB_COL_SIZE },
		{ "Quality", AXIS_WEB_COL_TEXT },
		{ "How it got there", AXIS_WEB_COL_TEXT },
		{ "Since", AXIS_WEB_COL_DATE },
		{ NULL, 0 }
	};
	HtmxNode *card = axis_web_storage_card("Files of this movie");
	HtmxNode *table = axis_web_table_new("film-files", columns);
	HtmxTbody *body = htmx_tbody_new();
	guint i;

	htmx_element_set_id(HTMX_ELEMENT(card), "files");
	if (notice != NULL)
		axis_web_notice_put(notice, card, "files");
	for (i = 0; i < detail->files->len; i++) {
		const AxisMovieFile *file = g_ptr_array_index(detail->files, i);
		AxisQuality quality = AXIS_QUALITY_UNKNOWN;
		HtmxTr *row = htmx_tr_new();
		HtmxTd *name = htmx_td_new();
		HtmxTd *how = htmx_td_new();
		g_autofree gchar *href = g_strdup_printf("/library/file/%" G_GINT64_FORMAT, file->file_id);
		g_autofree gchar *size = axis_size_format((guint64)MAX(file->size, 0));
		g_autofree gchar *root = g_strdup_printf("%s (%s)", file->root_name, file->volume_name);
		HtmxA *link = htmx_a_new_with_href(href);

		ADD(link, axis_web_browser_name_node(file->rel_path));
		ADD(name, link);
		if (g_strcmp0(file->state, "present") != 0)
			ADD(name, axis_web_browser_badge(g_strcmp0(file->state, "missing") == 0 ? "Missing from disk" :
				g_strcmp0(file->state, "suspected") == 0 ? "Possibly missing" : file->state, "badge-warn"));
		else if (file->away)
			/* Still counted: a disk that is away never means a file is gone. */
			ADD(name, axis_web_browser_badge("Disk offline", "badge-note"));
		if (file->identity_id > 0 && file->best && detail->files->len > 1)
			ADD(name, axis_web_storage_text("range-detail", "The copy its upgrades are judged by."));
		ADD(row, name);
		ADD(row, cell_text(root));
		ADD(row, cell_text(size));
		if (file->quality != NULL)
			axis_quality_from_name(file->quality, &quality);
		if (quality != AXIS_QUALITY_UNKNOWN) {
			ADD(row, cell_text(axis_quality_label(quality)));
		} else if (file->resolution != NULL) {
			g_autofree gchar *unknown = g_strdup_printf("Unknown source, %s", file->resolution);

			ADD(row, cell_text(unknown));
		} else {
			ADD(row, cell_text("Unknown"));
		}
		if (file->import_id > 0) {
			g_autofree gchar *import = g_strdup_printf("/imports/%" G_GINT64_FORMAT, file->import_id);

			ADD(how, axis_web_browser_link(import, g_strcmp0(file->mode, "move") == 0 ||
				g_strcmp0(file->mode, "move_copy") == 0 ? "Moved" : g_strcmp0(file->mode, "copy") == 0 ? "Copied" :
				g_strcmp0(file->mode, "hardlink") == 0 ? "Hardlinked" : "Imported"));
			/* Its release is known (the import's download): an administrator may
			 * block it for the future.  The page it leads to writes nothing, and
			 * blocking never touches this file, but while the download's grab is
			 * still open it also gives up on the rest of the download
			 * (add-block-release). */
			if (admin && !f->library_only && f->id > 0) {
				g_autofree gchar *block = g_strdup_printf("/films/block-release?import=%" G_GINT64_FORMAT,
					file->import_id);
				HtmxSpan *line = htmx_span_new();

				htmx_element_add_class(HTMX_ELEMENT(line), "range-detail");
				htmx_element_add_class(HTMX_ELEMENT(line), "film-file-block");
				ADD(line, axis_web_browser_link(block, file->grab_open ? "Block and stop the rest of this download" :
					"Block this release"));
				ADD(how, line);
			}
		} else if (file->identity_id > 0) {
			g_autofree gchar *unit = g_strdup_printf("/identify/units/%" G_GINT64_FORMAT, file->identity_id);

			ADD(how, axis_web_browser_link(unit, g_strcmp0(file->identity_how, "auto") == 0 ? "Identified" :
				g_strcmp0(file->identity_how, "imported") == 0 ? "Imported" : "Confirmed"));
		}
		/* Made by a person (a grab by force), and kept by hand: said, with
		 * the way to let it go (add-force-grab-and-assign). */
		if (file->hand_how != NULL) {
			g_autofree gchar *by = g_strdup_printf("%s by %s.", g_strcmp0(file->hand_how, "forced") == 0 ?
				"From a release grabbed by force" : g_strcmp0(file->hand_how, "assigned") == 0 ?
				"Assigned by hand" : g_strcmp0(file->hand_how, "brought") == 0 ? "Migrated from Radarr" :
				"Linked by hand", file->hand_by != NULL ? file->hand_by : "a person");

			ADD(how, axis_web_storage_text("range-detail film-file-hand", by));
			/* Let go by a person: it is upgraded as any file (import-from-arr). */
			if (g_strcmp0(file->hand_how, "brought") == 0 && !file->kept)
				ADD(how, axis_web_storage_text("range-detail film-file-brought", "Axis upgrades it by the movie's "
					"quality profile, as any file."));
		}
		/* A brought file is kept from the start, by no person's choice. */
		if (file->kept && file->kept_brought)
			ADD(how, axis_web_storage_text("range-detail film-file-kept", "Axis does not replace it: no automatic "
				"search and no Search now does, until you let Axis upgrade it again."));
		else if (file->kept)
			ADD(how, axis_web_storage_text("range-detail film-file-kept", "Kept by hand: no automatic search and no "
				"Search now replaces it."));
		if (admin && !f->library_only && f->id > 0 && g_strcmp0(file->state, "present") == 0 &&
		    (file->kept || file->hand_how != NULL)) {
			g_autofree gchar *action = g_strdup_printf("/films/%" G_GINT64_FORMAT "/links/keep", f->id);
			g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT, file->file_id);
			HtmxNode *form = axis_web_auth_post_form(request, action);

			htmx_element_add_class(HTMX_ELEMENT(form), "film-button");
			ADD(form, axis_web_auth_hidden("file", id));
			ADD(form, axis_web_auth_hidden("keep", file->kept ? "no" : "yes"));
			ADD(form, axis_web_auth_button(file->kept ? "Let Axis upgrade it again" : "Keep it by hand",
				"button-small"));
			ADD(how, form);
		}
		ADD(row, how);
		ADD(row, cell_time(file->linked_us));
		ADD(body, row);
	}
	ADD(table, body);
	ADD(card, table);
	{
		g_autofree gchar *copies = copies_text(f->n_copies);

		if (copies != NULL)
			ADD(card, axis_web_storage_text("help film-copies", copies));
	}
	return card;
}

/* ── Forms ──────────────────────────────────────────────────────── */

/* @css: (nullable) a further class of the field ("field-wide"). */
static HtmxNode *
select_field(
	const gchar *id,
	const gchar *label_text,
	const gchar *css,
	HtmxNode    *select,
	const gchar *help
){
	HtmxDiv *div = htmx_div_new();
	HtmxLabel *label = htmx_label_new_for(id);

	htmx_element_add_class(HTMX_ELEMENT(div), "field");
	if (css != NULL)
		htmx_element_add_class(HTMX_ELEMENT(div), css);
	htmx_node_set_text_content(HTMX_NODE(label), label_text);
	htmx_element_set_id(HTMX_ELEMENT(select), id);
	ADD(div, label);
	ADD(div, select);
	if (help != NULL)
		ADD(div, axis_web_storage_text("help", help));
	return HTMX_NODE(div);
}

static HtmxNode *
check_field(
	const gchar *id,
	const gchar *label_text,
	const gchar *name,
	gboolean     checked
){
	HtmxDiv *div = htmx_div_new();
	HtmxInput *input = htmx_input_new(HTMX_INPUT_CHECKBOX);
	HtmxLabel *label = htmx_label_new_for(id);

	htmx_element_add_class(HTMX_ELEMENT(div), "field-check");
	htmx_input_set_name(input, name);
	htmx_input_set_value(input, "yes");
	htmx_element_set_id(HTMX_ELEMENT(input), id);
	if (checked)
		htmx_element_set_attribute(HTMX_ELEMENT(input), "checked", "checked");
	htmx_node_set_text_content(HTMX_NODE(label), label_text);
	ADD(div, input);
	ADD(div, label);
	return HTMX_NODE(div);
}

static HtmxNode *
profile_select(
	const AxisMoviesChoices *choices,
	gint64                   current
){
	HtmxSelect *select = htmx_select_new_with_name("profile");
	guint i;

	for (i = 0; i < choices->profiles->len; i++) {
		const AxisMoviesChoice *c = g_ptr_array_index(choices->profiles, i);
		g_autofree gchar *value = axis_movies_int_text(c->id);
		HtmxOption *option;

		if (!c->usable && c->id != current)
			continue;
		option = htmx_option_new_with_value(value, c->label);
		htmx_option_set_selected(option, current > 0 ? c->id == current : g_strcmp0(c->slug, choices->default_slug) == 0);
		ADD(select, option);
	}
	return HTMX_NODE(select);
}

static HtmxNode *
root_select(
	const AxisMoviesChoices *choices,
	gint64                   current
){
	HtmxSelect *select = htmx_select_new_with_name("root");
	gboolean first = TRUE;
	guint i;

	for (i = 0; i < choices->roots->len; i++) {
		const AxisMoviesChoice *c = g_ptr_array_index(choices->roots, i);
		g_autofree gchar *value = axis_movies_int_text(c->id);
		g_autofree gchar *label = NULL;
		HtmxOption *option;

		/* A root on a disk that is away, or mounted read-only, is listed
		 * with why, and chosen only to keep a film's own. */
		if (c->why_not != NULL && c->id != current) {
			label = g_strdup_printf("%s: cannot be written to now, %s", c->label, c->why_not);
			option = htmx_option_new_with_value(value, label);
			htmx_element_set_attribute(HTMX_ELEMENT(option), "disabled", "disabled");
			ADD(select, option);
			continue;
		}
		option = htmx_option_new_with_value(value, c->label);
		htmx_option_set_selected(option, current > 0 ? c->id == current : first);
		first = FALSE;
		ADD(select, option);
	}
	return HTMX_NODE(select);
}

static HtmxNode *
availability_select(AxisMovieAvailability current)
{
	HtmxSelect *select = htmx_select_new_with_name("availability");
	guint i;

	for (i = 0; i <= AXIS_MOVIE_RELEASED; i++) {
		HtmxOption *option = htmx_option_new_with_value(axis_movie_availability_name((AxisMovieAvailability)i),
			axis_movie_availability_label((AxisMovieAvailability)i));

		htmx_option_set_selected(option, (AxisMovieAvailability)i == current);
		ADD(select, option);
	}
	return HTMX_NODE(select);
}

static guint
usable_roots(const AxisMoviesChoices *choices)
{
	guint n = 0;
	guint i;

	for (i = 0; i < choices->roots->len; i++)
		n += ((const AxisMoviesChoice *)g_ptr_array_index(choices->roots, i))->why_not == NULL ? 1 : 0;
	return n;
}

static const AxisMoviesChoice *first_profile(const AxisMoviesChoices *choices);
static const AxisMoviesChoice *first_root(const AxisMoviesChoices *choices);

/*
 * The add form's fields other than its button, in a disclosure: on a phone
 * it is closed, its summary names the profile and destination the form
 * holds, and the button adds with them; wider, axis.css shows the fields in
 * the form's row as they always were.
 */
static HtmxNode *
add_options(
	HtmxRequest             *request,
	const AxisMoviesChoices *choices
){
	const AxisMoviesChoice *profile;
	const AxisMoviesChoice *root;
	HtmxDetails *details = htmx_details_new();
	HtmxSummary *summary = htmx_summary_new();
	HtmxSpan *chosen = htmx_span_new();
	g_autofree gchar *text = NULL;

	axis_movies_web_defaults(request, choices, &profile, &root);
	text = g_strdup_printf("%s, %s", profile != NULL ? profile->label : "", root != NULL ? root->label : "");
	htmx_element_add_class(HTMX_ELEMENT(details), "add-options");
	htmx_node_set_text_content(HTMX_NODE(summary), "Options");
	htmx_element_add_class(HTMX_ELEMENT(chosen), "add-options-chosen");
	htmx_node_set_text_content(HTMX_NODE(chosen), text);
	ADD(summary, chosen);
	ADD(details, summary);
	return HTMX_NODE(details);
}

HtmxNode *
axis_movies_web_add_form(
	HtmxRequest             *request,
	gint64                   title_id,
	const AxisMoviesChoices *choices
){
	HtmxDiv *box = htmx_div_new();
	HtmxNode *form;
	HtmxNode *options;
	g_autofree gchar *title = axis_movies_int_text(title_id);

	htmx_element_add_class(HTMX_ELEMENT(box), "film-add");
	ADD(box, axis_web_storage_text("film-add-lead", "Not among your movies. Add it, and Axis looks for a release the "
		"quality profile accepts and sends it to a download client."));
	if (usable_roots(choices) == 0) {
		HtmxP *line = htmx_p_new();

		htmx_element_add_class(HTMX_ELEMENT(line), "help");
		htmx_node_set_text_content(HTMX_NODE(line), "There is no library folder for movies on a disk that is online, so "
			"there is nowhere for it to go. Add one first: ");
		ADD(line, axis_web_browser_link("/storage", "Disks"));
		ADD(box, line);
		return HTMX_NODE(box);
	}
	form = axis_web_auth_post_form(request, "/films");
	htmx_element_add_class(HTMX_ELEMENT(form), "film-form");
	ADD(form, axis_web_auth_hidden("title", title));
	/* Opened from a list (Discover, the search results): back to it, at the
	 * title's card. */
	{
		g_autofree gchar *from = NULL;
		g_autofree gchar *card = NULL;

		if (axis_web_back_from_add(request, title_id, &from, &card)) {
			ADD(form, axis_web_auth_hidden(AXIS_WEB_BACK_FIELD, from));
			ADD(form, axis_web_auth_hidden(AXIS_WEB_PLACE_AT, card));
		}
	}
	options = add_options(request, choices);
	/* Chosen at first: the person's own choice for Adds (the bar above a
	 * list); what is chosen here wins for this one. */
	{
		const AxisMoviesChoice *profile;
		const AxisMoviesChoice *root;

		axis_movies_web_defaults(request, choices, &profile, &root);
		ADD(options, select_field("film-add-profile", "Quality profile", NULL, profile_select(choices,
			profile != NULL ? profile->id : 0), NULL));
		ADD(options, select_field("film-add-root", "Destination", "field-wide", root_select(choices,
			root != NULL ? root->id : 0), NULL));
	}
	ADD(options, select_field("film-add-availability", "Search from", NULL,
		availability_select(choices->availability), NULL));
	ADD(options, check_field("film-add-monitored", "Monitored", "monitored", TRUE));
	ADD(options, check_field("film-add-search", "Search now", "search", TRUE));
	ADD(form, options);
	ADD(form, axis_web_auth_button("Add to Movies", "button-primary"));
	ADD(box, form);
	ADD(box, axis_web_storage_text("help", library_note));
	return HTMX_NODE(box);
}

/* What profile_select() and root_select() select at first, or %NULL. */
static const AxisMoviesChoice *
first_profile(const AxisMoviesChoices *choices)
{
	const AxisMoviesChoice *first = NULL;
	guint i;

	for (i = 0; i < choices->profiles->len; i++) {
		const AxisMoviesChoice *c = g_ptr_array_index(choices->profiles, i);

		if (!c->usable)
			continue;
		if (g_strcmp0(c->slug, choices->default_slug) == 0)
			return c;
		if (first == NULL)
			first = c;
	}
	return first;
}

static const AxisMoviesChoice *
first_root(const AxisMoviesChoices *choices)
{
	guint i;

	for (i = 0; i < choices->roots->len; i++) {
		const AxisMoviesChoice *c = g_ptr_array_index(choices->roots, i);

		if (c->why_not == NULL)
			return c;
	}
	return NULL;
}

/* ── The person's defaults for Add from a list ──────────────────── */

/* The choice @id of @list a new movie can take now (a profile that can be
 * used, a root it can be imported into), or %NULL. */
static const AxisMoviesChoice *
usable_choice(
	const GPtrArray *list,
	gint64           id
){
	guint i;

	for (i = 0; id > 0 && i < list->len; i++) {
		const AxisMoviesChoice *c = g_ptr_array_index(list, i);

		if (c->id == id)
			return c->usable && c->why_not == NULL ? c : NULL;
	}
	return NULL;
}

/* Whether the person chose a folder or a profile that cannot be taken now
 * (its disk away or read-only, or removed): their Adds use the
 * installation's until they choose again, and the bar says so. */
static gboolean
defaults_lapsed(
	HtmxRequest             *request,
	const AxisMoviesChoices *choices
){
	g_autoptr(JsonObject) mine = axis_web_request_add_defaults(request, AXIS_MOVIES_WEB_DEFAULTS_KIND);
	gint64 root;
	gint64 profile;

	if (mine == NULL)
		return FALSE;
	root = json_object_get_int_member_with_default(mine, "root", 0);
	profile = json_object_get_int_member_with_default(mine, "profile", 0);
	return (root > 0 && usable_choice(choices->roots, root) == NULL) ||
		(profile > 0 && usable_choice(choices->profiles, profile) == NULL);
}

void
axis_movies_web_defaults(
	HtmxRequest              *request,
	const AxisMoviesChoices  *choices,
	const AxisMoviesChoice  **profile,
	const AxisMoviesChoice  **root
){
	g_autoptr(JsonObject) mine = request != NULL ? axis_web_request_add_defaults(request,
		AXIS_MOVIES_WEB_DEFAULTS_KIND) : NULL;
	const AxisMoviesChoice *chosen;

	*profile = first_profile(choices);
	*root = first_root(choices);
	if (mine == NULL)
		return;
	/* (A choice that cannot be taken now is no choice: the installation's
	 * defaults, as before any was made; the bar says so.) */
	if (defaults_lapsed(request, choices))
		return;
	chosen = usable_choice(choices->profiles, json_object_get_int_member_with_default(mine, "profile", 0));
	if (chosen != NULL)
		*profile = chosen;
	chosen = usable_choice(choices->roots, json_object_get_int_member_with_default(mine, "root", 0));
	if (chosen != NULL)
		*root = chosen;
}

/* What a card's Options line says the Add uses. */
static gchar *
chosen_words(
	const AxisMoviesChoice *profile,
	const AxisMoviesChoice *root
){
	return g_strdup_printf("%s, %s", profile != NULL ? profile->label : "", root != NULL ? root->label : "");
}

/* The classes by which the bar's answer finds every card's pieces. */
#define CHOSEN_CLASS  "add-default-chosen-movies"
#define PROFILE_CLASS "add-default-profile-movies"
#define ROOT_CLASS    "add-default-root-movies"

static HtmxNode *
classed_hidden(
	const gchar *name,
	gint64       value,
	const gchar *css
){
	g_autofree gchar *text = axis_movies_int_text(value);
	HtmxNode *input = axis_web_auth_hidden(name, text);

	htmx_element_add_class(HTMX_ELEMENT(input), css);
	return input;
}

static HtmxNode *
chosen_span(
	const AxisMoviesChoice *profile,
	const AxisMoviesChoice *root
){
	g_autofree gchar *words = chosen_words(profile, root);
	HtmxSpan *span = htmx_span_new();

	htmx_element_add_class(HTMX_ELEMENT(span), "quick-add-chosen");
	htmx_element_add_class(HTMX_ELEMENT(span), CHOSEN_CLASS);
	htmx_node_set_text_content(HTMX_NODE(span), words);
	return HTMX_NODE(span);
}

/* The folder select of the bar: every movie folder, those on a disk Axis
 * cannot write to now shown and not to be chosen, with why. */
static HtmxNode *
bar_root_select(
	const AxisMoviesChoices *choices,
	const AxisMoviesChoice  *current
){
	HtmxSelect *select = htmx_select_new_with_name("root");
	guint i;

	for (i = 0; i < choices->roots->len; i++) {
		const AxisMoviesChoice *c = g_ptr_array_index(choices->roots, i);
		g_autofree gchar *value = axis_movies_int_text(c->id);
		g_autofree gchar *label = c->why_not == NULL ? g_strdup(c->label) :
			g_strdup_printf("%s: cannot be written to now, %s", c->label, c->why_not);
		HtmxOption *option = htmx_option_new_with_value(value, label);

		if (c->why_not != NULL)
			htmx_element_set_attribute(HTMX_ELEMENT(option), "disabled", "disabled");
		htmx_option_set_selected(option, current != NULL && c->id == current->id);
		ADD(select, option);
	}
	return HTMX_NODE(select);
}

HtmxNode *
axis_movies_web_defaults_bar(
	HtmxRequest             *request,
	const AxisMoviesChoices *choices,
	const gchar             *page
){
	const AxisMoviesChoice *profile;
	const AxisMoviesChoice *root;
	HtmxNode *form = axis_web_auth_post_form(request, "/films/defaults");
	g_autofree gchar *here = page == NULL ? axis_web_back_here(request) : NULL;
	HtmxNode *back = page != NULL || here != NULL ? axis_web_auth_hidden(AXIS_WEB_BACK_FIELD, page != NULL ? page :
		here) : NULL;
	HtmxNode *button = axis_web_auth_button("Save as my defaults", "button-small");

	axis_movies_web_defaults(request, choices, &profile, &root);
	htmx_element_set_id(HTMX_ELEMENT(form), AXIS_MOVIES_WEB_DEFAULTS_BAR);
	htmx_element_add_class(HTMX_ELEMENT(form), "add-defaults");
	/* With htmx a change is stored at once and the cards follow; without,
	 * the button. */
	htmx_element_set_attribute(HTMX_ELEMENT(form), "hx-post", "/films/defaults");
	htmx_element_set_attribute(HTMX_ELEMENT(form), "hx-trigger", "change");
	htmx_element_set_attribute(HTMX_ELEMENT(form), "hx-swap", "none");
	ADD(form, select_field("add-defaults-movies-root", "New movies go to", "add-defaults-field",
		bar_root_select(choices, root), NULL));
	ADD(form, select_field("add-defaults-movies-profile", "with", "add-defaults-field", profile_select(choices,
		profile != NULL ? profile->id : 0), NULL));
	if (back != NULL)
		ADD(form, back);
	ADD(form, axis_web_auth_hidden(AXIS_WEB_PLACE_AT, AXIS_MOVIES_WEB_DEFAULTS_BAR));
	ADD(form, button);
	if (defaults_lapsed(request, choices))
		ADD(form, axis_web_storage_text("help add-defaults-lapsed", "The folder or profile you chose cannot be used "
			"now (its disk is away or read-only, or it was removed): Adds use these until you choose again."));
	return form;
}

HtmxResponse *
axis_movies_web_defaults_answer(
	HtmxRequest             *request,
	const AxisMoviesChoices *choices
){
	g_autoptr(HtmxFragment) fragment = htmx_fragment_new();
	g_autofree gchar *page = axis_web_back_posted(request);
	const AxisMoviesChoice *profile;
	const AxisMoviesChoice *root;
	HtmxNode *node;

	axis_movies_web_defaults(request, choices, &profile, &root);
	axis_web_activity_add_oob(HTMX_NODE(fragment), axis_movies_web_defaults_bar(request, choices,
		page != NULL ? page : ""));
	if (profile != NULL && root != NULL) {
		node = classed_hidden("profile", profile->id, PROFILE_CLASS);
		axis_web_activity_add_oob(HTMX_NODE(fragment), node);
		htmx_element_set_attribute(HTMX_ELEMENT(node), "hx-swap-oob", "outerHTML:." PROFILE_CLASS);
		node = classed_hidden("root", root->id, ROOT_CLASS);
		axis_web_activity_add_oob(HTMX_NODE(fragment), node);
		htmx_element_set_attribute(HTMX_ELEMENT(node), "hx-swap-oob", "outerHTML:." ROOT_CLASS);
		node = chosen_span(profile, root);
		axis_web_activity_add_oob(HTMX_NODE(fragment), node);
		htmx_element_set_attribute(HTMX_ELEMENT(node), "hx-swap-oob", "outerHTML:." CHOSEN_CLASS);
	}
	return axis_web_activity_respond(HTMX_NODE(fragment));
}

HtmxNode *
axis_movies_web_quick_add(
	HtmxRequest             *request,
	gint64                   title_id,
	const AxisMoviesChoices *choices
){
	const AxisMoviesChoice *profile;
	const AxisMoviesChoice *root;
	/* Its page knows the list it came from, and comes back to this card. */
	g_autofree gchar *page = axis_web_back_title_href(request, title_id);
	HtmxDiv *box = htmx_div_new();
	HtmxNode *back = NULL;
	HtmxA *options;

	/* Where the person's Adds go (the bar above the list). */
	axis_movies_web_defaults(request, choices, &profile, &root);
	htmx_element_add_class(HTMX_ELEMENT(box), "quick-add");
	if (profile != NULL && root != NULL)
		back = axis_web_back_add_field(request, title_id);
	if (back != NULL) {
		g_autofree gchar *title = axis_movies_int_text(title_id);
		g_autofree gchar *tip = g_strdup_printf("Add to Movies with the quality profile %s and the destination %s, "
			"and search for it now", profile->label, root->label);
		HtmxNode *form = axis_web_auth_post_form(request, "/films");
		HtmxNode *button = axis_web_auth_button("Add to Movies", "button-primary button-small");

		htmx_element_add_class(HTMX_ELEMENT(form), "inline-form");
		ADD(form, axis_web_auth_hidden("title", title));
		ADD(form, classed_hidden("profile", profile->id, PROFILE_CLASS));
		ADD(form, classed_hidden("root", root->id, ROOT_CLASS));
		ADD(form, axis_web_auth_hidden("availability", axis_movie_availability_name(choices->availability)));
		ADD(form, axis_web_auth_hidden("monitored", "yes"));
		ADD(form, axis_web_auth_hidden("search", "yes"));
		ADD(form, back);
		/* Back at this card (Discover's id for it), or where the list says
		 * when the card is gone after the add. */
		{
			g_autofree gchar *card = axis_web_back_add_place(request, title_id);

			ADD(form, axis_web_auth_hidden(AXIS_WEB_PLACE_AT, card));
		}
		htmx_element_set_attribute(HTMX_ELEMENT(button), "title", tip);
		ADD(form, button);
		ADD(box, form);
	}
	/* The title page's form chooses everything (and says what is missing). */
	options = htmx_a_new_with_href(page);
	htmx_element_add_class(HTMX_ELEMENT(options), "quick-add-options");
	htmx_node_set_text_content(HTMX_NODE(options), "Add with options");
	htmx_element_set_attribute(HTMX_ELEMENT(options), "title", back != NULL ?
		"Choose the quality profile, the destination and the rest on the movie's page" :
		"There is no usable quality profile or library folder for movies: the movie's page says what is needed");
	/* Closed, it says what the Add uses. */
	if (back != NULL)
		ADD(options, chosen_span(profile, root));
	ADD(box, options);
	return HTMX_NODE(box);
}

/* A button that opens to a confirming form (no script: <details>). */
static HtmxNode *
confirm_form(
	HtmxRequest *request,
	const gchar *action,
	const gchar *opener,
	const gchar *confirm_id,
	const gchar *confirm_text,
	const gchar *button
){
	HtmxDetails *details = htmx_details_new();
	HtmxSummary *summary = htmx_summary_new();
	HtmxNode *form = axis_web_auth_post_form(request, action);

	htmx_element_add_class(HTMX_ELEMENT(details), "film-confirm");
	htmx_node_set_text_content(HTMX_NODE(summary), opener);
	ADD(details, summary);
	htmx_element_add_class(HTMX_ELEMENT(form), "film-confirm-form");
	ADD(form, check_field(confirm_id, confirm_text, "confirm", FALSE));
	ADD(form, axis_web_auth_button(button, "button-danger button-small"));
	ADD(details, form);
	return HTMX_NODE(details);
}

void
axis_movies_web_topics(
	const AxisMovieFacts *facts,
	gchar               **topics
){
	g_autofree gchar *key = g_strdup_printf(AXIS_MOVIES_SEARCH_KEY_PREFIX "%" G_GINT64_FORMAT, facts->id);

	topics[0] = axis_web_activity_topic(key);
	topics[1] = axis_web_activity_title_topic(facts->title_id);
	topics[2] = NULL;
}

/* The words of the film's search in hand, or of its last search lately.  (Its
 * download is said by the state's own line and bar, just above.) */
static gchar *
film_words(
	const AxisMoviesShown *shown,
	AxisWebActivityTone   *tone
){
	const AxisMoviesWork *work = shown->work;

	*tone = AXIS_WEB_ACTIVITY_RUNNING;
	if (work == NULL)
		return NULL;
	if (work->search != NULL || work->list != NULL) {
		const AxisJobLive *job = work->search != NULL ? work->search : work->list;
		g_autofree gchar *state = axis_web_activity_state(job, work->now_us);

		*tone = job->running ? AXIS_WEB_ACTIVITY_RUNNING : AXIS_WEB_ACTIVITY_WAITING;
		return g_strdup_printf("A %ssearch of the movie is %s.", job == work->list ? "manual " : "", state);
	}
	if (work->ended_us > 0 && work->ended_us >= work->list_ended_us) {
		g_autofree gchar *ago = axis_web_activity_ago(work->ended_us, work->now_us);
		g_autofree gchar *outcome = axis_web_activity_outcome(work->outcome, work->outcome_text, work->n_found, tone);

		return g_strdup_printf("Searched %s: it %s.", ago, outcome);
	}
	if (work->list_ended_us > 0) {
		g_autofree gchar *ago = axis_web_activity_ago(work->list_ended_us, work->now_us);
		g_autofree gchar *outcome = axis_web_activity_outcome("listed", NULL, work->list_n_found, tone);

		return g_strdup_printf("A manual search ended %s: it %s.", ago, outcome);
	}
	return NULL;
}

HtmxNode *
axis_movies_web_film_line(const AxisMoviesShown *shown)
{
	g_autofree gchar *id = g_strdup_printf("activity-film-%" G_GINT64_FORMAT, shown->facts->id);
	AxisWebActivityTone tone = AXIS_WEB_ACTIVITY_RUNNING;
	g_autofree gchar *text = film_words(shown, &tone);

	return axis_web_activity_line(id, tone, text);
}

gchar *
axis_movies_web_answer(
	const gchar           *done,
	const AxisMoviesShown *shown
){
	const AxisMoviesWork *work = shown != NULL ? shown->work : NULL;
	AxisWebActivityTone tone;

	if (work == NULL)
		return NULL;
	if (g_strcmp0(done, "already_searching") == 0 && work->search != NULL) {
		g_autofree gchar *state = axis_web_activity_state(work->search, work->now_us);

		return g_strdup_printf("Nothing new was queued: a search of the movie is %s.", state);
	}
	if ((g_strcmp0(done, "searched_recently") == 0 || g_strcmp0(done, "already_searching") == 0) &&
	    work->ended_us > 0) {
		g_autofree gchar *ago = axis_web_activity_ago(work->ended_us, work->now_us);
		g_autofree gchar *outcome = axis_web_activity_outcome(work->outcome, work->outcome_text, work->n_found, &tone);
		g_autofree gchar *again = axis_web_activity_clock(work->ended_us + (gint64)AXIS_ACQUIRE_SEARCH_AGAIN_S *
			G_USEC_PER_SEC, work->now_us);

		return g_strdup_printf("Nothing new was queued: a search of the movie ended %s and %s. You can ask again "
			"from %s.", ago, outcome, again);
	}
	return NULL;
}

HtmxNode *
axis_movies_web_actions(
	HtmxRequest           *request,
	const AxisMoviesShown *shown
){
	const AxisMovieFacts *f = shown->facts;
	const AxisMoviesWork *work = shown->work;
	HtmxDiv *row = htmx_div_new();
	g_autofree gchar *id = g_strdup_printf("film-actions-%" G_GINT64_FORMAT, f->id);
	g_autofree gchar *search = g_strdup_printf("/films/%" G_GINT64_FORMAT "/search", f->id);
	g_autofree gchar *give_up = g_strdup_printf("/films/%" G_GINT64_FORMAT "/give-up", f->id);
	gint64 again_us = (gint64)AXIS_ACQUIRE_SEARCH_AGAIN_S * G_USEC_PER_SEC;

	htmx_element_set_id(HTMX_ELEMENT(row), id);
	htmx_element_add_class(HTMX_ELEMENT(row), "film-actions");
	/* Brought over and held: the way to let Axis search for it by itself
	 * (searching for it here, or a manual search, ends the hold too). */
	if (axis_movie_hold_stops(f->search_hold)) {
		g_autofree gchar *start = g_strdup_printf("/films/%" G_GINT64_FORMAT "/start-searching", f->id);

		ADD(row, button_form(request, start, NULL, NULL, "Let Axis search for it", "button-small"));
	}
	/* While its search is in hand, or just ended, the button says so. */
	if (work != NULL && work->search != NULL)
		ADD(row, axis_web_activity_busy_button(work->search->running ? "Searching" : "Waiting for its turn",
			"A search of the movie is in hand: the line above says where it stands."));
	else if (work != NULL && work->ended_us > 0 && work->now_us - work->ended_us < again_us)
		ADD(row, axis_web_activity_busy_button("Searched a moment ago", "The line above says what it found; you "
			"can search again in a minute."));
	else if (shown->state != AXIS_MOVIE_NOT_AVAILABLE && shown->state != AXIS_MOVIE_DOWNLOADING &&
	    shown->state != AXIS_MOVIE_TO_IMPORT && shown->state != AXIS_MOVIE_SEARCHING && !axis_movie_in_flight(f))
		ADD(row, button_form(request, search, NULL, NULL, g_strcmp0(f->search_outcome, "nothing_found") == 0 ?
			"Search again now" : "Search for this movie", "button-primary button-small"));
	/* A person's look, whatever the rules say now: it grabs nothing.  While
	 * one is in hand or just ended, its page instead. */
	if (work != NULL && (work->list != NULL || (work->list_ended_us > 0 && work->now_us - work->list_ended_us <
	    again_us))) {
		g_autofree gchar *href = axis_web_manual_path("/films", f->id, "movie", -1);
		HtmxA *link = htmx_a_new_with_href(href);

		htmx_element_add_class(HTMX_ELEMENT(link), "button");
		htmx_element_add_class(HTMX_ELEMENT(link), "button-small");
		htmx_node_set_text_content(HTMX_NODE(link), work->list != NULL ? "Manual search in hand: see it" :
			"Listed a moment ago: see the manual search");
		ADD(row, link);
	} else {
		ADD(row, axis_web_manual_button(request, "/films", f->id, "movie", -1, "Manual search"));
	}
	if (f->grab_id != 0)
		ADD(row, confirm_form(request, give_up, "Give up on this download", "film-give-up-confirm",
			"Put it on the blocklist, remove it from its download client asking it to delete its files (what SABnzbd keeps of "
			"a finished job, Axis deletes where the rules allow), and look for the next best",
			"Yes, give up on this download"));
	/* Its files and their links, changed by hand (add-force-grab-and-assign):
	 * offered whether or not it has a file, since linking one is how a file
	 * Axis could not match becomes this movie's. */
	{
		g_autofree gchar *links = g_strdup_printf("/films/%" G_GINT64_FORMAT "/links", f->id);

		ADD(row, axis_web_browser_link(links, "Files and links"));
	}
	/* (Removing it is under Settings for this movie.) */
	return HTMX_NODE(row);
}

HtmxNode *
axis_movies_web_manage_form(
	HtmxRequest             *request,
	const AxisMovieFacts    *f,
	const AxisMoviesChoices *choices
){
	HtmxDiv *box = htmx_div_new();
	HtmxNode *form;
	g_autofree gchar *title = axis_movies_int_text(f->title_id);

	htmx_element_add_class(HTMX_ELEMENT(box), "film-add");
	ADD(box, axis_web_storage_text("film-add-lead", "Axis lists this movie because its file is on disk, and does "
		"nothing with it: nothing is searched for, downloaded, upgraded, renamed or moved. To have Axis look after "
		"it, choose a quality profile and manage it."));
	if (usable_roots(choices) == 0) {
		HtmxP *line = htmx_p_new();

		htmx_element_add_class(HTMX_ELEMENT(line), "help");
		htmx_node_set_text_content(HTMX_NODE(line), "To manage it, a library folder for movies on a disk that is online "
			"is needed, where a better copy would go. Add one first: ");
		ADD(line, axis_web_browser_link("/storage", "Disks"));
		ADD(box, line);
		return HTMX_NODE(box);
	}
	form = axis_web_auth_post_form(request, "/films");
	htmx_element_add_class(HTMX_ELEMENT(form), "film-form");
	ADD(form, axis_web_auth_hidden("title", title));
	ADD(form, axis_web_auth_hidden("availability", axis_movie_availability_name(choices->availability)));
	ADD(form, select_field("film-manage-profile", "Quality profile", NULL, profile_select(choices, 0), NULL));
	/* Where a better copy goes: the root its file is in, when that can be. */
	ADD(form, select_field("film-manage-root", "Destination", "field-wide", root_select(choices, f->root_id), NULL));
	ADD(form, check_field("film-manage-monitored", "Monitored for upgrades", "monitored", FALSE));
	ADD(form, axis_web_auth_button("Manage this movie", "button-primary"));
	ADD(box, form);
	ADD(box, axis_web_storage_text("help", "Managed, it keeps a history and can be searched for by hand; nothing is "
		"searched for now, since its file is there. Monitored for upgrades, Axis also looks for a better copy while "
		"its file is below the cutoff of its quality profile. Its file stays where it is either way."));
	return HTMX_NODE(box);
}

/* ── A film's cards ─────────────────────────────────────────────── */

static HtmxNode *
download_card(const AxisMovieFacts *f)
{
	HtmxNode *card = axis_web_storage_card(f->grab_completed_us > 0 ||
		(f->download_known && f->download_state == AXIS_DOWNLOAD_COMPLETED) ? "Latest download" : "Latest download");
	HtmxNode *facts = axis_web_browser_facts_new();
	g_autofree gchar *when = axis_web_format_time(f->grab_us, "");
	g_autofree gchar *grabbed = f->grab_manual ? g_strdup_printf("%s, by hand by %s", when, f->grab_by != NULL ?
		f->grab_by : "an administrator") : g_strdup_printf("%s, by the search", when);

	axis_web_browser_fact_node(facts, "Release", axis_web_browser_name_node(f->grab_release));
	if (f->grab_quality != NULL) {
		AxisQuality q;

		axis_web_browser_fact(facts, "Quality", axis_quality_from_name(f->grab_quality, &q) ? axis_quality_label(q) :
			f->grab_quality);
	}
	axis_web_browser_fact(facts, "Grabbed", grabbed);
	if (f->client_name != NULL)
		axis_web_browser_fact(facts, "Download client", f->client_name);
	if (f->download_known) {
		g_autofree gchar *state = NULL;

		if (f->download_size > 0 && f->download_done >= 0) {
			g_autofree gchar *size = axis_quality_format_size(f->download_size);

			state = g_strdup_printf("%s, %.0f%% of %s", axis_download_state_label(f->download_state),
				CLAMP((gdouble)f->download_done / (gdouble)f->download_size, 0.0, 1.0) * 100.0, size);
		} else {
			state = g_strdup(axis_download_state_label(f->download_state));
		}
		axis_web_browser_fact(facts, "At the download client", state);
	} else if (f->download_id == 0) {
		axis_web_browser_fact(facts, "At the download client", "Being handed over");
	}
	if (f->download_rel_path != NULL) {
		g_autofree gchar *where = g_strdup_printf("%s, at %s", f->download_volume != NULL ? f->download_volume :
			"a disk", f->download_rel_path);

		axis_web_browser_fact_node(facts, "Files", axis_web_breakable(where));
	} else if (f->download_client_path != NULL) {
		axis_web_browser_fact_node(facts, "Path at the download client", axis_web_breakable(f->download_client_path));
	}
	if (f->download_message != NULL)
		axis_web_browser_fact(facts, "Message from the download client", f->download_message);
	ADD(card, facts);
	if (f->grab_completed_us > 0 || (f->download_known && f->download_state == AXIS_DOWNLOAD_COMPLETED))
		ADD(card, axis_web_storage_text("help", "Until the import, the files stay where the client put them. The "
			"import moves the movie's file and the files it takes with it (subtitles, extras) into the library: one "
			"rename each when the download is on the library's disk, otherwise a verified copy, the download's file "
			"being deleted once the import is recorded. A torrent's client is first asked to let go of it, without its "
			"files, and nothing is moved until it has; a torrent left seeding is hardlinked instead, and with \"Files "
			"taken from a download\" set to hardlink every import is, as before. Then a Usenet job is removed from its "
			"client, and what the import did not take (an .nfo, a sample) is deleted by Axis with the download's "
			"folder, inside the client's folder for Axis and only where the rules allow it. If something else deletes "
			"the download's files before the import, the movie is wanted again. The import's page says what was "
			"done."));
	return card;
}

static const AxisWebColumn release_columns[] = {
	{ "Release", AXIS_WEB_COL_NAME },
	{ "Quality", AXIS_WEB_COL_TEXT },
	{ "Size", AXIS_WEB_COL_SIZE },
	{ "Found at", AXIS_WEB_COL_TEXT },
	{ "Verdict", AXIS_WEB_COL_STATE },
	{ "", AXIS_WEB_COL_ACTIONS },
	{ NULL, 0 }
};

static gint
compare_candidates(
	gconstpointer a,
	gconstpointer b
){
	const AxisMovieCandidate *x = *(const AxisMovieCandidate *const *)a;
	const AxisMovieCandidate *y = *(const AxisMovieCandidate *const *)b;

	if (x->accepted != y->accepted)
		return x->accepted ? -1 : 1;
	if (x->accepted && x->rank != y->rank)
		return x->rank < y->rank ? -1 : 1;
	return x->position < y->position ? -1 : x->position > y->position;
}

static HtmxNode *
verdict_badge(const AxisMovieCandidate *c)
{
	if (c->note != NULL && strcmp(c->note, "Grabbed.") == 0)
		return axis_web_browser_badge("Grabbed", "badge-running");
	if (c->accepted) {
		g_autofree gchar *text = g_strdup_printf("Accepted, rank %u", c->rank);

		return axis_web_browser_badge(text, "badge-succeeded");
	}
	if (c->blocklisted)
		return axis_web_browser_badge("Blocklisted", "badge-warn");
	return axis_web_browser_badge(c->safety ? "Ruled out" : "Rejected", NULL);
}

static HtmxNode *
release_row(
	HtmxRequest              *request,
	const AxisMovieFacts     *f,
	const AxisMovieCandidate *c,
	gboolean                  admin
){
	HtmxTr *row = htmx_tr_new();
	HtmxTd *name = htmx_td_new();
	HtmxBdi *bdi = htmx_bdi_new();
	g_autofree gchar *size = c->size_bytes >= 0 ? axis_quality_format_size(c->size_bytes) : NULL;
	g_autofree gchar *where = NULL;
	g_autofree gchar *age = c->published_us > 0 ? axis_movies_web_age(c->published_us) : NULL;
	AxisQuality q;
	guint i;

	if (c->accepted)
		htmx_element_add_class(HTMX_ELEMENT(row), "release-accepted");
	ADD(bdi, axis_web_breakable(c->name));
	ADD(name, bdi);
	if (c->reasons->len > 0 || c->note != NULL || (c->accepted && c->match_text != NULL)) {
		HtmxUl *list = htmx_ul_new();

		htmx_element_add_class(HTMX_ELEMENT(list), "release-reasons");
		for (i = 0; i < c->reasons->len; i++) {
			const AxisMovieReason *r = g_ptr_array_index(c->reasons, i);
			HtmxLi *item = htmx_li_new();
			g_autofree gchar *css = g_strdup_printf("reason-%s", r->kind != NULL ? r->kind : "profile");

			htmx_element_add_class(HTMX_ELEMENT(item), css);
			htmx_node_set_text_content(HTMX_NODE(item), r->text);
			ADD(list, item);
		}
		if (c->accepted && c->match_text != NULL && c->reasons->len == 0) {
			HtmxLi *item = htmx_li_new();
			g_autofree gchar *text = g_strdup_printf("%c%s.", g_ascii_toupper(c->match_text[0]), c->match_text + 1);

			htmx_element_add_class(HTMX_ELEMENT(item), "reason-match");
			htmx_node_set_text_content(HTMX_NODE(item), text);
			ADD(list, item);
		}
		if (c->note != NULL) {
			HtmxLi *item = htmx_li_new();

			htmx_element_add_class(HTMX_ELEMENT(item), "reason-note");
			htmx_node_set_text_content(HTMX_NODE(item), c->note);
			ADD(list, item);
		}
		ADD(name, list);
	}
	ADD(row, name);
	ADD(row, cell_text(c->quality != NULL && axis_quality_from_name(c->quality, &q) ? axis_quality_label(q) :
		"Unknown"));
	ADD(row, cell_text(size != NULL ? size : ""));
	/* The indexer, and under it the kind with what tells its availability. */
	if (c->protocol == AXIS_DOWNLOAD_TORRENT && c->seeders >= 0)
		where = g_strdup_printf("Torrent, %d %s", c->seeders, c->seeders == 1 ? "seeder" : "seeders");
	else if (c->protocol == AXIS_DOWNLOAD_TORRENT)
		where = g_strdup("Torrent");
	else
		where = g_strdup_printf("Usenet%s%s", age != NULL && age[0] != '\0' ? ", " : "", age != NULL ? age : "");
	{
		HtmxTd *cell = htmx_td_new();
		HtmxSpan *indexer = htmx_span_new();
		HtmxSpan *detail = htmx_span_new();

		htmx_node_set_text_content(HTMX_NODE(indexer), c->indexer != NULL ? c->indexer : "An indexer");
		htmx_element_add_class(HTMX_ELEMENT(detail), "range-detail");
		htmx_node_set_text_content(HTMX_NODE(detail), where);
		ADD(cell, indexer);
		ADD(cell, detail);
		ADD(row, cell);
	}
	ADD(row, cell_node(verdict_badge(c)));
	if (admin && !c->safety && !c->blocklisted && f->grab_id == 0 && f->other_live == 0 && f->kept_by != NULL &&
	    f->n_files > 0) {
		/* A file kept by hand is replaced only through a confirmation that
		 * names it (Manual search), never by one click. */
		g_autofree gchar *href = axis_web_manual_path("/films", f->id, "movie", -1);
		HtmxTd *cell = htmx_td_new();
		HtmxSpan *words = htmx_span_new();

		htmx_node_set_text_content(HTMX_NODE(words), f->kept_brought ? "Its file was migrated from Radarr; Axis "
			"does not replace it: to replace it, use " : "Its file is kept by hand: to replace it, use ");
		ADD(cell, words);
		ADD(cell, axis_web_browser_link(href, "Manual search"));
		ADD(row, HTMX_NODE(cell));
	} else if (admin && !c->safety && !c->blocklisted && f->grab_id == 0 && f->other_live == 0) {
		g_autofree gchar *action = g_strdup_printf("/films/%" G_GINT64_FORMAT "/grab", f->id);
		g_autofree gchar *id = axis_movies_int_text(c->id);

		/* A button either way; teal for the release the profile accepts, the
		 * one this row is for, and the plain one for overriding a verdict. */
		ADD(row, cell_node(button_form(request, action, "candidate", id, c->accepted ? "Grab" : "Grab it anyway",
			c->accepted ? "button-primary button-small" : "button-small")));
	} else {
		ADD(row, cell_text(""));
	}
	return HTMX_NODE(row);
}

static const gchar *
cause_label(const gchar *cause)
{
	if (g_strcmp0(cause, "scheduled") == 0)
		return "scheduled";
	if (g_strcmp0(cause, "retry") == 0)
		return "for the next best, after a failure";
	if (g_strcmp0(cause, "added") == 0)
		return "on adding";
	if (g_strcmp0(cause, "profile") == 0)
		return "after its quality profile changed";
	return "asked for";
}

static HtmxNode *
releases_card(
	HtmxRequest           *request,
	const AxisMoviesShown *shown,
	const AxisMovieDetail *detail,
	gboolean               admin,
	AxisWebNotice         *notice
){
	HtmxNode *card = axis_web_storage_card("Releases found by the last search");
	const AxisMovieSearchInfo *s = detail->search;
	g_autoptr(GPtrArray) order = g_ptr_array_new();
	guint i;

	htmx_element_set_id(HTMX_ELEMENT(card), "releases");
	if (notice != NULL)
		axis_web_notice_put(notice, card, "releases");
	/* A person's look has a page of its own: linked for an administrator,
	 * and for anyone once there is one. */
	if (admin || detail->manual) {
		g_autofree gchar *manual = axis_web_manual_path("/films", shown->facts->id, "movie", -1);
		HtmxP *line = htmx_p_new();

		htmx_element_add_class(HTMX_ELEMENT(line), "help");
		ADD(line, axis_web_browser_link(manual, "The last manual search"));
		ADD(card, line);
	}
	if (s == NULL) {
		ADD(card, axis_web_storage_text("help", "Not searched for yet. What a search finds is listed here, each "
			"release accepted or rejected with every reason."));
		return card;
	}
	{
		g_autofree gchar *when = axis_web_format_time(s->finished_us > 0 ? s->finished_us : s->started_us, "");
		g_autofree gchar *found = s->how != NULL ? g_strdup(s->how) : g_strdup_printf("%u found", s->n_found);
		g_autofree gchar *line = g_strdup_printf("Last search %s, %s%s%s. %s; %u accepted%s%s.", when,
			cause_label(s->cause), s->requested_by != NULL ? " by " : "", s->requested_by != NULL ? s->requested_by : "",
			found, s->n_accepted, s->n_compared > 1 ? "; names compared with " : "", s->n_compared > 1 ?
			(s->n_compared == 2 ? "one other movie of that name" : "other movies of that name") : "");

		ADD(card, axis_web_storage_text("film-search-line", line));
		if (s->finished_us == 0)
			ADD(card, axis_web_storage_text("help", "The search is being finished: the release it chose is being "
				"sent."));
		else if (s->outcome_text != NULL && g_strcmp0(s->outcome_text, shown->reason) != 0)
			/* What the search ended in, unless the film's state above already
			 * says exactly that: a reason is stated once. */
			ADD(card, axis_web_storage_text("film-outcome", s->outcome_text));
	}
	if (detail->candidates->len == 0)
		return card;
	for (i = 0; i < detail->candidates->len; i++)
		g_ptr_array_add(order, g_ptr_array_index(detail->candidates, i));
	g_ptr_array_sort(order, compare_candidates);
	{
		HtmxNode *table = axis_web_table_new("film-releases", release_columns);
		HtmxTbody *body = htmx_tbody_new();

		for (i = 0; i < order->len && i < AXIS_MOVIES_WEB_RELEASES_SHOWN; i++)
			ADD(body, release_row(request, shown->facts, g_ptr_array_index(order, i), admin));
		ADD(table, body);
		ADD(card, table);
	}
	if (order->len > AXIS_MOVIES_WEB_RELEASES_SHOWN) {
		g_autofree gchar *count = axis_web_count(order->len - AXIS_MOVIES_WEB_RELEASES_SHOWN, "more release",
			"more releases");
		g_autofree gchar *more = g_strdup_printf("And %s, rejected or ranked lower.", count);

		ADD(card, axis_web_storage_text("help", more));
	}
	{
		gboolean ruled_out = FALSE;
		gboolean rejected = FALSE;

		for (i = 0; i < order->len; i++) {
			const AxisMovieCandidate *c = g_ptr_array_index(order, i);

			ruled_out = ruled_out || c->safety;
			rejected = rejected || (!c->accepted && !c->safety && !c->blocklisted);
		}
		if (ruled_out)
			ADD(card, axis_web_storage_text("help", "\"Ruled out\": a fixed rule rejects it (not this movie, not "
				"certainly this movie, a sample, an archive or a program), and no one can grab it."));
		if (admin && rejected)
			ADD(card, axis_web_storage_text("help", "\"Grab it anyway\" sends a release the profile rejected, as a grab "
				"by hand. A search's releases can be grabbed for fifteen minutes; then search again."));
	}
	return card;
}

/* ── What puts a film right ─────────────────────────────────────── */

/* A line of words with a link in it (@before and @after nullable). */
static HtmxNode *
line_with_link(
	const gchar *css,
	const gchar *before,
	const gchar *href,
	const gchar *words,
	const gchar *after
){
	HtmxP *line = htmx_p_new();

	if (css != NULL)
		htmx_element_add_class(HTMX_ELEMENT(line), css);
	if (before != NULL) {
		HtmxSpan *span = htmx_span_new();

		htmx_node_set_text_content(HTMX_NODE(span), before);
		ADD(line, span);
	}
	ADD(line, axis_web_browser_link(href, words));
	if (after != NULL) {
		HtmxSpan *span = htmx_span_new();

		htmx_node_set_text_content(HTMX_NODE(span), after);
		ADD(line, span);
	}
	return HTMX_NODE(line);
}

/* A release the profile rejected that a person may take: not by a fixed
 * rule, not on the blocklist. */
static gboolean
rejected_by_profile(const AxisMovieCandidate *c)
{
	return !c->accepted && !c->safety && !c->blocklisted;
}

void
axis_movies_tried_clear(AxisMoviesTried *tried)
{
	g_clear_pointer(&tried->candidates, g_ptr_array_unref);
	g_clear_pointer(&tried->profile_ids, g_array_unref);
	g_clear_pointer(&tried->tries, g_ptr_array_unref);
}

void
axis_movies_web_try(
	AxisMovies              *self,
	const AxisMoviesShown   *shown,
	const AxisMovieDetail   *detail,
	const AxisMoviesChoices *choices,
	AxisMoviesTried         *tried
){
	g_autoptr(GError) error = NULL;
	guint i;

	memset(tried, 0, sizeof(*tried));
	if (shown == NULL || detail == NULL || choices == NULL || shown->kind != AXIS_ACQUIRE_ATTENTION_REJECTED ||
	    detail->candidates == NULL)
		return;
	tried->candidates = g_ptr_array_new();
	for (i = 0; i < detail->candidates->len; i++) {
		const AxisMovieCandidate *c = g_ptr_array_index(detail->candidates, i);

		if (rejected_by_profile(c))
			g_ptr_array_add(tried->candidates, (gpointer)c);
	}
	tried->profile_ids = g_array_new(FALSE, FALSE, sizeof(gint64));
	for (i = 0; i < choices->profiles->len; i++) {
		const AxisMoviesChoice *c = g_ptr_array_index(choices->profiles, i);

		if (c->usable || c->id == shown->facts->profile_id)
			g_array_append_val(tried->profile_ids, c->id);
	}
	if (tried->candidates->len > 0 && !axis_movies_try_profiles(self, shown->facts->id, tried->candidates,
	        tried->profile_ids, &tried->tries, &error))
		g_debug("films: the profiles could not be tried: %s", error->message);
}

static const AxisMoviesChoice *
profile_choice(
	const AxisMoviesChoices *choices,
	gint64                   id
){
	guint i;

	for (i = 0; choices->profiles != NULL && i < choices->profiles->len; i++) {
		const AxisMoviesChoice *c = g_ptr_array_index(choices->profiles, i);

		if (c->id == id)
			return c;
	}
	return NULL;
}

/* "Use another quality profile for this movie": each usable profile, with
 * what it makes of the releases the profile rejected (the decision a search
 * makes), saved through the film's own edit (its other settings kept),
 * which searches again. */
static HtmxNode *
profile_form(
	HtmxRequest             *request,
	const AxisMovieFacts    *f,
	const AxisMoviesChoices *choices,
	const AxisMoviesTried   *tried
){
	g_autofree gchar *action = g_strdup_printf("/films/%" G_GINT64_FORMAT "/edit", f->id);
	g_autofree gchar *root = axis_movies_int_text(f->root_id);
	HtmxNode *form = axis_web_auth_post_form(request, action);
	HtmxFieldset *set = htmx_fieldset_new();
	guint n = tried->candidates != NULL ? tried->candidates->len : 0;
	gint64 chosen = f->profile_id;
	guint i;

	htmx_element_add_class(HTMX_ELEMENT(form), "profile-choice");
	ADD(form, axis_web_auth_hidden("root", root));
	ADD(form, axis_web_auth_hidden("availability", axis_movie_availability_name(f->availability)));
	if (f->monitored)
		ADD(form, axis_web_auth_hidden("monitored", "yes"));
	for (i = 0; tried->tries != NULL && i < tried->tries->len && chosen == f->profile_id; i++) {
		const AxisAcquireProfileTry *t = g_ptr_array_index(tried->tries, i);

		if (t->n_accepted > 0 && t->profile_id != f->profile_id)
			chosen = t->profile_id;
	}
	htmx_element_add_class(HTMX_ELEMENT(set), "profile-group");
	ADD(set, htmx_legend_new_with_text("Use another quality profile for this movie"));
	for (i = 0; tried->profile_ids != NULL && i < tried->profile_ids->len; i++) {
		gint64 id = g_array_index(tried->profile_ids, gint64, i);
		const AxisMoviesChoice *c = profile_choice(choices, id);
		const AxisAcquireProfileTry *t = tried->tries != NULL && i < tried->tries->len ?
			g_ptr_array_index(tried->tries, i) : NULL;
		g_autofree gchar *value = axis_movies_int_text(id);
		g_autofree gchar *input_id = g_strdup_printf("profile-choice-%" G_GINT64_FORMAT, id);
		g_autofree gchar *note = NULL;
		HtmxDiv *div;
		HtmxInput *input;
		HtmxLabel *label;
		HtmxSpan *name;
		HtmxSpan *what;

		if (c == NULL)
			continue;
		div = htmx_div_new();
		input = htmx_input_new(HTMX_INPUT_RADIO);
		label = htmx_label_new_for(input_id);
		name = htmx_span_new();
		what = htmx_span_new();
		if (t == NULL || n == 0)
			note = g_strdup("");
		else if (t->n_accepted > 0)
			note = n == 1 ? g_strdup(": accepts this release") : g_strdup_printf(": accepts %u of these %u releases",
				t->n_accepted, n);
		else
			note = g_strdup_printf(": %s%s", n == 1 ? "rejects it" : "rejects them", t->reason != NULL ? ", " : "");
		htmx_element_add_class(HTMX_ELEMENT(div), "field-check");
		htmx_element_add_class(HTMX_ELEMENT(div), t != NULL && t->n_accepted > 0 ? "profile-accepts" :
			"profile-rejects");
		htmx_input_set_name(input, "profile");
		htmx_input_set_value(input, value);
		htmx_element_set_id(HTMX_ELEMENT(input), input_id);
		if (id == chosen)
			htmx_element_set_attribute(HTMX_ELEMENT(input), "checked", "checked");
		htmx_node_set_text_content(HTMX_NODE(name), c->label);
		ADD(label, name);
		if (id == f->profile_id) {
			HtmxSpan *now = htmx_span_new();

			htmx_node_set_text_content(HTMX_NODE(now), " (the movie's profile now)");
			ADD(label, now);
		}
		htmx_element_add_class(HTMX_ELEMENT(what), "profile-verdict");
		htmx_node_set_text_content(HTMX_NODE(what), note);
		ADD(label, what);
		if (t != NULL && n > 0 && t->n_accepted == 0 && t->reason != NULL) {
			HtmxSpan *why = htmx_span_new();

			htmx_element_add_class(HTMX_ELEMENT(why), "profile-verdict");
			htmx_node_set_text_content(HTMX_NODE(why), t->reason);
			ADD(label, why);
		}
		ADD(div, input);
		ADD(div, label);
		ADD(set, div);
	}
	ADD(form, set);
	ADD(form, axis_web_auth_button("Use this profile and search again", "button-primary button-small"));
	return form;
}

HtmxNode *
axis_movies_web_resolve(
	HtmxRequest             *request,
	const AxisMoviesShown   *shown,
	const AxisMovieDetail   *detail,
	const AxisMoviesChoices *choices,
	const AxisMoviesTried   *tried
){
	const AxisMovieFacts *f = shown->facts;
	gboolean admin = axis_web_request_is_admin(request);
	HtmxDiv *box;

	if (f->library_only || shown->kind == AXIS_ACQUIRE_ATTENTION_NONE || shown->kind == AXIS_ACQUIRE_ATTENTION_IMPORT ||
	    shown->kind == AXIS_ACQUIRE_ATTENTION_LEFT_AT_CLIENT)
		return NULL;
	box = htmx_div_new();
	htmx_element_set_id(HTMX_ELEMENT(box), "resolve");
	htmx_element_add_class(HTMX_ELEMENT(box), "resolve-box");
	ADD(box, heading(3, "What you can do"));
	switch (shown->kind) {
	case AXIS_ACQUIRE_ATTENTION_REJECTED: {
		const AxisMovieCandidate *only = NULL;
		guint n = 0;
		guint i;

		for (i = 0; detail != NULL && detail->candidates != NULL && i < detail->candidates->len; i++) {
			const AxisMovieCandidate *c = g_ptr_array_index(detail->candidates, i);

			if (rejected_by_profile(c)) {
				n++;
				only = c;
			}
		}
		if (!admin) {
			ADD(box, axis_web_storage_text("help", "An administrator can grab a release anyway, or use another quality "
				"profile for this movie, which searches again with it."));
			break;
		}
		/* (a) Grab it anyway, with the guards of a grab by hand. */
		if (n > 0 && f->kept_by != NULL && f->n_files > 0) {
			g_autofree gchar *href = axis_web_manual_path("/films", f->id, "movie", -1);

			ADD(box, line_with_link("help", f->kept_brought ? "The movie's file in the library was migrated from "
				"Radarr and Axis does not replace it, so no single click does. To replace it, use " :
				"The movie's file in the library is kept by hand, so no single click "
				"replaces it. To replace it, use ", href, "Manual search", ", which asks you to confirm."));
		} else if (n == 1 && f->grab_id == 0 && f->other_live == 0) {
			g_autofree gchar *action = g_strdup_printf("/films/%" G_GINT64_FORMAT "/grab", f->id);
			g_autofree gchar *id = axis_movies_int_text(only->id);
			HtmxDiv *line = htmx_div_new();

			htmx_element_add_class(HTMX_ELEMENT(line), "film-actions");
			ADD(line, button_form(request, action, "candidate", id, "Grab it anyway", "button-small"));
			ADD(line, axis_web_storage_text("help", "Sends the release the profile rejected, as a grab by hand."));
			ADD(box, line);
		} else if (n > 1) {
			ADD(box, line_with_link("help", "Take one of the releases with Grab it anyway: it is sent as a grab by "
				"hand, whatever the profile says. ", "#releases", "See the releases", NULL));
		}
		/* (b) Another profile, marked by what it makes of them. */
		if (choices != NULL && tried != NULL && tried->profile_ids != NULL && tried->profile_ids->len > 0)
			ADD(box, profile_form(request, f, choices, tried));
		/* (c) The profile's own limits. */
		if (choices != NULL) {
			const AxisMoviesChoice *c = profile_choice(choices, f->profile_id);

			if (c != NULL && c->slug != NULL) {
				g_autofree gchar *href = g_strdup_printf("/quality/%s", c->slug);
				g_autofree gchar *words = g_strdup_printf("Change the limits of %s itself, on its page", c->label);
				HtmxP *line = htmx_p_new();

				htmx_element_add_class(HTMX_ELEMENT(line), "help");
				ADD(line, axis_web_browser_link(href, words));
				ADD(box, line);
			}
		}
		break;
	}
	case AXIS_ACQUIRE_ATTENTION_EXHAUSTED:
		ADD(box, line_with_link("help", "The releases that failed before are on the movie's blocklist. Take one off "
			"it, or clear the blocklist, and search again; or wait for new releases. ", "#blocklist",
			"See the blocklist", NULL));
		break;
	case AXIS_ACQUIRE_ATTENTION_NO_INDEXER:
	case AXIS_ACQUIRE_ATTENTION_INDEXER_ERROR:
		if (admin)
			ADD(box, line_with_link("help", NULL, "/indexers", "Look at the indexers", ", then use Search for this movie."));
		else
			ADD(box, axis_web_storage_text("help", "An administrator can look at the indexers and search again."));
		break;
	case AXIS_ACQUIRE_ATTENTION_NO_CLIENT:
	case AXIS_ACQUIRE_ATTENTION_CLIENT_ERROR:
		if (admin)
			ADD(box, line_with_link("help", NULL, "/downloads/clients", "Look at the download clients",
				", then use Search for this movie."));
		else
			ADD(box, axis_web_storage_text("help", "An administrator can look at the download clients and search "
				"again."));
		break;
	case AXIS_ACQUIRE_ATTENTION_NO_ROOT:
	case AXIS_ACQUIRE_ATTENTION_ROOT_OFFLINE:
	case AXIS_ACQUIRE_ATTENTION_PROFILE_UNUSABLE:
		ADD(box, line_with_link("help", NULL, "#settings", shown->kind == AXIS_ACQUIRE_ATTENTION_PROFILE_UNUSABLE ?
			"Choose another quality profile under Settings for this movie" :
			"Choose another destination under Settings for this movie", NULL));
		break;
	default:
		ADD(box, axis_web_storage_text("help", admin ? "Search again with Search for this movie." :
			"An administrator can search again."));
		break;
	}
	return HTMX_NODE(box);
}

static const AxisWebColumn block_columns[] = {
	{ "Release", AXIS_WEB_COL_NAME },
	{ "How it got here", AXIS_WEB_COL_TEXT },
	{ "Why", AXIS_WEB_COL_WIDE },
	{ "Since", AXIS_WEB_COL_DATE },
	{ "By", AXIS_WEB_COL_TEXT },
	{ "", AXIS_WEB_COL_ACTIONS },
	{ NULL, 0 }
};

/* An entry's origin: a failure's, or a person's (add-block-release). */
static gchar *
block_origin(const AxisMovieBlock *b)
{
	const gchar *who = b->created_by != NULL ? b->created_by : "an administrator";

	if (g_strcmp0(b->reason, AXIS_ACQUIRE_BLOCK_PERSON) == 0)
		return g_strdup_printf("Blocked by %s", who);
	if (g_strcmp0(b->reason, "given_up") == 0)
		return g_strdup_printf("Download given up on by %s", who);
	return g_strdup("Failed");
}

static HtmxNode *
blocklist_card(
	HtmxRequest           *request,
	const AxisMovieFacts  *f,
	const AxisMovieDetail *detail,
	gboolean               admin,
	AxisWebNotice         *notice
){
	HtmxNode *card = axis_web_storage_card("Blocklist of this movie");
	g_autofree gchar *action = g_strdup_printf("/films/%" G_GINT64_FORMAT "/blocklist/clear", f->id);
	HtmxNode *table;
	HtmxTbody *body;
	guint i;

	htmx_element_set_id(HTMX_ELEMENT(card), "blocklist");
	if (notice != NULL)
		axis_web_notice_put(notice, card, "blocklist");
	if (detail->blocklist->len == 0) {
		ADD(card, axis_web_storage_text("help", "Nothing is on this movie's blocklist. A release whose download "
			"fails, stalls or disappears goes on it and is not tried again; so does a release an administrator "
			"blocks, from a manual search or from a download."));
		return card;
	}
	ADD(card, axis_web_storage_text("help", "No search takes a release that is listed here. A release that failed is "
		"blocked from the indexer it came from, and by its hash when it is a torrent. A release a person blocked is "
		"blocked by its name from every indexer. Unblock takes one release off the list."));
	table = axis_web_table_new("film-blocklist", block_columns);
	body = htmx_tbody_new();
	for (i = 0; i < detail->blocklist->len; i++) {
		const AxisMovieBlock *b = g_ptr_array_index(detail->blocklist, i);
		HtmxTr *row = htmx_tr_new();
		g_autofree gchar *origin = block_origin(b);

		ADD(row, name_cell(b->release_name));
		ADD(row, cell_text(origin));
		ADD(row, cell_breakable(b->reason_text != NULL ? b->reason_text : b->reason));
		ADD(row, cell_time(b->created_us));
		ADD(row, cell_text(b->created_by != NULL ? b->created_by : "Axis"));
		if (admin) {
			g_autofree gchar *id = axis_movies_int_text(b->id);

			ADD(row, cell_node(button_form(request, action, "entry", id, "Unblock", "button-small")));
		} else {
			ADD(row, cell_text(""));
		}
		ADD(body, row);
	}
	ADD(table, body);
	ADD(card, table);
	if (admin && detail->blocklist->len > 1)
		ADD(card, button_form(request, action, NULL, NULL, "Clear the blocklist", "button-small"));
	return card;
}

static HtmxNode *
options_card(
	HtmxRequest             *request,
	const AxisMovieFacts    *f,
	const AxisMoviesChoices *choices,
	gboolean                 admin,
	AxisWebNotice           *notice
){
	HtmxNode *card = axis_web_storage_card("Settings for this movie");
	g_autofree gchar *added = axis_web_format_time(f->added_us, "");
	g_autofree gchar *added_text = g_strdup_printf("%s%s%s", added, f->added_by != NULL ? ", by " : "",
		f->added_by != NULL ? f->added_by : "");

	/* The card the state's "Settings for this movie" leads to, and where its
	 * Save comes back to. */
	htmx_element_set_id(HTMX_ELEMENT(card), "settings");
	if (notice != NULL)
		axis_web_notice_put(notice, card, "settings");
	if (admin && choices != NULL) {
		g_autofree gchar *action = g_strdup_printf("/films/%" G_GINT64_FORMAT "/edit", f->id);
		g_autofree gchar *remove = g_strdup_printf("/films/%" G_GINT64_FORMAT "/remove", f->id);
		HtmxNode *form = axis_web_auth_post_form(request, action);
		HtmxDiv *row = htmx_div_new();
		HtmxNode *remove_form;

		htmx_element_add_class(HTMX_ELEMENT(form), "film-form");
		ADD(form, select_field("film-edit-profile", "Quality profile", NULL, profile_select(choices, f->profile_id),
			NULL));
		ADD(form, select_field("film-edit-root", "Destination", "field-wide", root_select(choices, f->root_id), NULL));
		ADD(form, select_field("film-edit-availability", "Search from", NULL, availability_select(f->availability),
			NULL));
		ADD(form, check_field("film-edit-monitored", "Monitored", "monitored", f->monitored));
		ADD(form, axis_web_auth_button("Save", NULL));
		ADD(card, form);
		ADD(card, axis_web_storage_text("help", "A new quality profile searches for a monitored movie again at once, "
			"with the new profile's rules. A new destination alone searches for nothing."));
		ADD(card, axis_web_storage_text("as-of", added_text));
		/* The card's one action that removes, last and set apart.  With its
		 * file on disk it stays listed, not managed: say so before. */
		htmx_element_add_class(HTMX_ELEMENT(row), "film-actions");
		htmx_element_add_class(HTMX_ELEMENT(row), "settings-actions");
		remove_form = confirm_form(request, remove, "Remove this movie", "film-remove-confirm", f->n_files > 0 ?
			"Stop managing this movie: forget its searches and its blocklist. No file is touched; while its file is on "
			"disk and recognised by identification, it stays in Movies as In library, not managed" :
			"Forget this movie, its searches and its blocklist (no file is touched)", "Yes, remove this movie");
		htmx_element_add_class(HTMX_ELEMENT(remove_form), "film-confirm-danger");
		htmx_element_add_class(HTMX_ELEMENT(remove_form), "actions-apart");
		ADD(row, remove_form);
		ADD(card, row);
		return card;
	}
	{
		HtmxNode *facts = axis_web_browser_facts_new();

		axis_web_browser_fact(facts, "Quality profile", f->profile_name);
		axis_web_browser_fact(facts, "Destination", f->root_name != NULL ? f->root_name : "None");
		axis_web_browser_fact(facts, "Search from", axis_movie_availability_label(f->availability));
		axis_web_browser_fact(facts, "Monitored", f->monitored ? "Yes" : "No");
		axis_web_browser_fact(facts, "Added", added_text);
		ADD(card, facts);
	}
	return card;
}

static const gchar *
event_label(const gchar *kind)
{
	static const struct
	{
		const gchar *kind;
		const gchar *label;
	} labels[] = {
		{ "added", "Added" },
		{ "removed", "Removed" },
		{ "changed", "Changed" },
		{ "searched", "Searched" },
		{ "skipped", "Not searched" },
		{ "grabbed", "Grabbed" },
		{ "not_sent", "Not sent" },
		{ "failed", "Failed" },
		{ "blocklisted", "Blocklisted" },
		{ "unblocked", "Off the blocklist" },
		{ "removal", "Removed at the download client" },
		{ "completed", "Downloaded" },
		{ "available", "Availability changed" },
		{ "abandoned", "Given up" },
		{ "identified", "Found on disk" },
		{ "unidentified", "No longer found on disk" },
		{ "imported", "Imported" },
		{ "import_blocked", "Import blocked" },
		{ "import_waiting", "Import waits" },
		{ "import_action", "Import decision" },
		{ "upgraded", "Upgraded" },
		{ NULL, NULL }
	};
	guint i;

	for (i = 0; kind != NULL && labels[i].kind != NULL; i++) {
		if (strcmp(labels[i].kind, kind) == 0)
			return labels[i].label;
	}
	return kind != NULL ? kind : "";
}

static const AxisWebColumn event_columns[] = {
	{ "When", AXIS_WEB_COL_DATE },
	{ "What", AXIS_WEB_COL_TEXT },
	{ "Details", AXIS_WEB_COL_WIDE },
	{ "By", AXIS_WEB_COL_TEXT },
	{ NULL, 0 }
};

static HtmxNode *
history_card(const AxisMovieDetail *detail)
{
	HtmxNode *card = axis_web_storage_card("History");
	HtmxNode *table = axis_web_table_new("film-events", event_columns);
	HtmxTbody *body = htmx_tbody_new();
	guint i;

	for (i = 0; i < detail->events->len; i++) {
		const AxisMovieEvent *e = g_ptr_array_index(detail->events, i);
		HtmxTr *row = htmx_tr_new();

		ADD(row, cell_time(e->created_us));
		ADD(row, cell_text(event_label(e->kind)));
		ADD(row, cell_breakable(e->text));
		ADD(row, cell_text(e->by_user != NULL ? e->by_user : "Axis"));
		ADD(body, row);
	}
	ADD(table, body);
	ADD(card, table);
	if (detail->events->len >= AXIS_MOVIES_WEB_EVENTS_SHOWN)
		ADD(card, axis_web_browser_link("/films/history", "History of every movie"));
	/* The foot of the last card of the film's own: what its states cannot
	 * yet know about the disk. */
	ADD(card, axis_web_storage_text("help", library_note));
	return card;
}

HtmxNode *
axis_movies_web_detail(
	HtmxRequest             *request,
	const AxisMoviesShown   *shown,
	const AxisMovieDetail   *detail,
	const AxisMoviesChoices *choices,
	AxisWebNotice           *notice
){
	const AxisMovieFacts *f = shown->facts;
	gboolean admin = axis_web_request_is_admin(request);
	HtmxDiv *box = htmx_div_new();

	htmx_element_add_class(HTMX_ELEMENT(box), "film-detail");
	if (detail->files != NULL && detail->files->len > 0)
		ADD(box, library_card(request, detail, f, admin, notice));
	/* A movie on disk Axis does not manage: its files, and what its title
	 * kept of a film removed before; nothing else is Axis's. */
	if (f->library_only) {
		if (detail->events != NULL && detail->events->len > 0)
			ADD(box, history_card(detail));
		return HTMX_NODE(box);
	}
	if (f->grab_id != 0)
		ADD(box, download_card(f));
	ADD(box, releases_card(request, shown, detail, admin, notice));
	ADD(box, blocklist_card(request, f, detail, admin, notice));
	ADD(box, options_card(request, f, choices, admin, notice));
	ADD(box, history_card(detail));
	return HTMX_NODE(box);
}

/* ── The Films list ─────────────────────────────────────────────── */

/* What the Wanted page reads (all the wanted films, at once). */
typedef struct
{
	gboolean        filtered;
	AxisMovieState  filter;
	GPtrArray      *films;	/* AxisMoviesShown */
	guint           counts[AXIS_MOVIE_N_STATES];
	GError         *error;
} GridWork;

static void
grid_work_free(gpointer data)
{
	GridWork *work = data;

	g_clear_pointer(&work->films, g_ptr_array_unref);
	g_clear_error(&work->error);
	g_free(work);
}

/*
 * The list: one page of the films in the view the account chose, read as
 * axis_movies_list_page() reads it (the counts, the page's ids, the page's
 * facts: never every film's).
 */
typedef struct
{
	gint64              user_id;
	gchar              *state;	/* a state's name, or NULL */
	gchar              *name;
	gchar              *sort;	/* as asked, or NULL */
	gchar              *view_asked;
	gchar              *after;
	gchar              *before;
	gchar              *done;
	gboolean            identify_on;	/* the Identify pages are there */
	AxisWebView         view;
	AxisMoviesListPage  page;
	GPtrArray          *shown;	/* AxisMoviesShown, the page's */
	GError             *error;
} ListWork;

static void
list_work_free(gpointer data)
{
	ListWork *work = data;

	g_free(work->state);
	g_free(work->name);
	g_free(work->sort);
	g_free(work->view_asked);
	g_free(work->after);
	g_free(work->before);
	g_free(work->done);
	axis_movies_list_page_clear(&work->page);
	g_clear_pointer(&work->shown, g_ptr_array_unref);
	g_clear_error(&work->error);
	g_free(work);
}

static void
list_run(
	AxisMovies *self,
	gpointer    data
){
	ListWork *work = data;
	AxisMoviesListQuery query;
	AxisMovieState state;
	AxisMovieRules rules;

	axis_web_view_choose(self->pool, work->user_id, "films", work->view_asked, &work->view);
	memset(&query, 0, sizeof(query));
	if (work->state != NULL && axis_movie_state_from_name(work->state, &state))
		query.states = 1u << state;
	query.name = work->name;
	query.sort = AXIS_MOVIES_SORT_ADDED;
	if (work->sort != NULL)
		axis_movies_sort_from_name(work->sort, &query.sort);
	query.limit = axis_web_view_page_size(work->view);
	query.after = work->after;
	query.before = work->before;
	if (!axis_movies_list_page(self, &query, &work->page, &work->error))
		return;
	axis_movies_get_rules(self, &rules);
	work->shown = g_ptr_array_new_with_free_func((GDestroyNotify)axis_movies_shown_free);
	while (work->page.films->len > 0)
		g_ptr_array_add(work->shown, axis_movies_shown_new(g_ptr_array_steal_index(work->page.films, 0), &rules));
	/* The page's posters, in one look at the store whatever their number:
	 * what is kept is shown; the rest is fetched in the background. */
	if (work->shown->len > 0) {
		guint n = work->shown->len;
		g_autofree const gchar **providers = g_new0(const gchar *, n);
		g_autofree const gchar **refs = g_new0(const gchar *, n);
		g_autofree gchar **posters = g_new0(gchar *, n);
		guint i;

		for (i = 0; i < n; i++) {
			const AxisMoviesShown *shown = g_ptr_array_index(work->shown, i);

			providers[i] = shown->facts->provider;
			refs[i] = shown->facts->poster_ref;
		}
		axis_meta_artwork_find_many(self->meta, providers, refs, n, AXIS_META_ART_POSTER_SMALL, TRUE, posters);
		for (i = 0; i < n; i++)
			((AxisMoviesShown *)g_ptr_array_index(work->shown, i))->poster = posters[i];
	}
}

/* The film's quality in words: its best file's, or what was grabbed. */
static const gchar *
quality_words(const AxisMovieFacts *f)
{
	AxisQuality grabbed = AXIS_QUALITY_UNKNOWN;

	if (f->n_files > 0 && f->file_quality != AXIS_QUALITY_UNKNOWN)
		return axis_quality_label(f->file_quality);
	if (f->n_files > 0 && f->file_resolution != AXIS_RELEASE_RESOLUTION_NONE)
		return axis_quality_unknown_source_label(f->file_resolution);
	if (f->grab_id != 0 && f->grab_quality != NULL && axis_quality_from_name(f->grab_quality, &grabbed))
		return axis_quality_label(grabbed);
	return NULL;
}

/* "2 copies", or %NULL for one. */
static gchar *
copies_words(const AxisMovieFacts *f)
{
	return f->n_copies > 1 ? g_strdup_printf("%u copies", f->n_copies) : NULL;
}

/* The overview's line of facts: the profile (or that Axis found it on disk),
 * the quality, the size, the copies. */
static gchar *
facts_line(const AxisMovieFacts *f)
{
	GString *line = g_string_new(NULL);
	const gchar *quality = quality_words(f);
	g_autofree gchar *copies = copies_words(f);

	if (f->library_only)
		g_string_append(line, "On disk");
	else if (f->profile_name != NULL)
		g_string_append_printf(line, "Quality profile %s", f->profile_name);
	if (quality != NULL)
		g_string_append_printf(line, "%s%s", line->len > 0 ? ", " : "", quality);
	if (f->size_bytes > 0) {
		g_autofree gchar *size = axis_size_format((guint64)f->size_bytes);

		g_string_append_printf(line, "%s%s", line->len > 0 ? ", " : "", size);
	}
	if (copies != NULL)
		g_string_append_printf(line, "%s%s", line->len > 0 ? ", " : "", copies);
	if (f->library_only)
		g_string_append_printf(line, "%snot managed", line->len > 0 ? ", " : "");
	else if (!f->monitored)
		g_string_append_printf(line, "%snot monitored", line->len > 0 ? ", " : "");
	return g_string_free(line, FALSE);
}

/* What identification holds back, and where to see it: the movies on disk
 * that are not in the list yet. */
static HtmxNode *
review_line(
	guint    review,
	guint    waiting,
	gboolean identify_on
){
	HtmxP *line = htmx_p_new();
	GString *text = g_string_new(NULL);

	htmx_element_add_class(HTMX_ELEMENT(line), "help");
	htmx_element_add_class(HTMX_ELEMENT(line), "film-review");
	if (review > 0) {
		g_autofree gchar *count = axis_web_count(review, "movie folder or file on disk is", "movie folders and files "
			"on disk are");

		g_string_append_printf(text, "%s not listed yet: identification was not sure which movie %s, and waits for "
			"a person to say. ", count, review == 1 ? "it is" : "each is");
	}
	if (waiting > 0) {
		g_autofree gchar *count = review > 0 ? axis_web_count(waiting, "more waits", "more wait") :
			axis_web_count(waiting, "movie folder or file on disk waits", "movie folders and files on disk wait");

		g_string_append_printf(text, "%s for TMDb, which did not answer; Identify asks again "
			"by itself. ", count);
	}
	if (identify_on) {
		htmx_node_set_text_content(HTMX_NODE(line), text->str);
		ADD(line, axis_web_browser_link("/identify", review > 0 ? "Review the waiting folders on the Identify page" :
			"See the waiting folders on the Identify page"));
	} else {
		g_string_append(text, "Identify is switched off: an administrator can switch it on under Modules.");
		htmx_node_set_text_content(HTMX_NODE(line), text->str);
	}
	g_string_free(text, TRUE);
	return HTMX_NODE(line);
}

static HtmxNode *
tab_item(
	const gchar *href,
	const gchar *label,
	guint        count,
	gboolean     current
){
	HtmxLi *item = htmx_li_new();
	HtmxA *link = htmx_a_new_with_href(href);
	HtmxSpan *text = htmx_span_new();
	HtmxSpan *figure = htmx_span_new();
	g_autofree gchar *number = g_strdup_printf("%u", count);

	if (current) {
		htmx_element_add_class(HTMX_ELEMENT(link), "current");
		htmx_element_set_attribute(HTMX_ELEMENT(link), "aria-current", "page");
	}
	htmx_node_set_text_content(HTMX_NODE(text), label);
	htmx_element_add_class(HTMX_ELEMENT(figure), "tab-count");
	htmx_node_set_text_content(HTMX_NODE(figure), number);
	ADD(link, text);
	ADD(link, figure);
	ADD(item, link);
	return HTMX_NODE(item);
}

/* The states in the order the filter shows them. */
static const AxisMovieState filter_order[] = {
	AXIS_MOVIE_WANTED, AXIS_MOVIE_SEARCHING, AXIS_MOVIE_DOWNLOADING, AXIS_MOVIE_TO_IMPORT, AXIS_MOVIE_IN_LIBRARY,
	AXIS_MOVIE_UPGRADE_WANTED, AXIS_MOVIE_ATTENTION, AXIS_MOVIE_NOT_AVAILABLE
};

static HtmxResponse *
list_done(
	AxisMovies  *self,
	HtmxRequest *request,
	gpointer     data
){
	static const gchar *const sorts[] = { "added", "title", "year", "size", NULL };
	static const gchar *const sort_labels[] = { "Added", "Title", "Year", "Size", NULL };
	ListWork *work = data;
	g_autoptr(HtmxDiv) content = htmx_div_new();
	AxisWebListAddress address;
	AxisWebTitleRow *rows;
	HtmxNav *tabs;
	HtmxUl *list;
	guint total = 0;
	guint n;
	guint i;
	const gchar *done;

	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	for (i = 0; i < AXIS_MOVIE_N_STATES; i++)
		total += work->page.counts[i];
	memset(&address, 0, sizeof(address));
	address.path = "/films";
	address.state = work->state;
	address.name = work->name;
	address.sort = work->sort;
	address.default_sort = "added";
	address.view = work->view;
	address.after = work->after;
	address.before = work->before;

	ADD(content, heading(1, "All movies"));
	done = axis_movies_web_done_message(work->done);
	if (done != NULL)
		ADD(content, axis_web_auth_notice(FALSE, done));

	/* The states, with how many of the films (by name, when filtered) are
	 * in each: from one grouped statement. */
	tabs = htmx_nav_new();
	htmx_element_add_class(HTMX_ELEMENT(tabs), "kind-tabs");
	htmx_element_set_attribute(HTMX_ELEMENT(tabs), "aria-label", "Movies by state");
	list = htmx_ul_new();
	ADD(list, axis_web_list_tab_new(&address, NULL, "All", total));
	for (i = 0; i < G_N_ELEMENTS(filter_order); i++) {
		AxisMovieState state = filter_order[i];
		const gchar *name = axis_movie_state_name(state);

		if (work->page.counts[state] == 0 && g_strcmp0(work->state, name) != 0)
			continue;
		ADD(list, axis_web_list_tab_new(&address, name, axis_movie_state_label(state), work->page.counts[state]));
	}
	ADD(tabs, list);
	ADD(content, tabs);
	/* What identification holds back is said, not left out. */
	if (work->page.review > 0 || work->page.waiting > 0)
		ADD(content, review_line(work->page.review, work->page.waiting, work->identify_on));
	ADD(content, axis_web_list_bar_new(&address, "movies", sorts, sort_labels));

	n = work->shown->len;
	if (n == 0) {
		HtmxP *line = htmx_p_new();

		htmx_element_add_class(HTMX_ELEMENT(line), "no-results");
		htmx_node_set_text_content(HTMX_NODE(line), total == 0 && work->name == NULL ? "No movies yet. Find one with "
			"the search box above and add it from its page; movies already on disk are listed here once "
			"identification has recognised them." : work->name != NULL ? "No movie's name matches." :
			"No movie is in this state. Choose All to see every movie.");
		ADD(content, line);
	} else {
		g_autoptr(GPtrArray) owned = g_ptr_array_new_with_free_func(g_free);

		rows = g_new0(AxisWebTitleRow, n);
		for (i = 0; i < n; i++) {
			const AxisMoviesShown *shown = g_ptr_array_index(work->shown, i);
			const AxisMovieFacts *f = shown->facts;
			gchar *href = g_strdup_printf("/title/%" G_GINT64_FORMAT, f->title_id);
			gchar *facts = facts_line(f);
			g_autofree gchar *copies = copies_words(f);

			g_ptr_array_add(owned, href);
			g_ptr_array_add(owned, facts);
			rows[i].href = href;
			/* A poster says when there is more than one copy, beside its year. */
			if (copies != NULL) {
				gchar *line = f->year > 0 ? g_strdup_printf("Movie, %d, %s", f->year, copies) :
					g_strdup_printf("Movie, %s", copies);

				g_ptr_array_add(owned, line);
				rows[i].line = line;
			}
			rows[i].title = f->title;
			rows[i].year = f->year;
			rows[i].kind = AXIS_META_KIND_FILM;
			rows[i].provider = f->provider;
			rows[i].poster_ref = f->poster_ref;
			rows[i].poster = shown->poster;
			rows[i].mark = mark_of(shown);
			rows[i].state = axis_movie_state_label(shown->state);
			/* A movie on disk, not managed: its facts line already says what is there, so only the
			 * offline note is left of the reason (the other views keep the whole sentence). */
			if (f->library_only && work->view == AXIS_WEB_VIEW_OVERVIEW) {
				gchar *away = axis_movie_library_away_note(f);

				if (away != NULL)
					g_ptr_array_add(owned, away);
				rows[i].reason = away;
			} else {
				rows[i].reason = shown->reason;
			}
			rows[i].attention = shown->state == AXIS_MOVIE_ATTENTION;
			/* What needs attention links to where it is put right. */
			if (rows[i].attention) {
				gchar *where = attention_href(shown);

				g_ptr_array_add(owned, where);
				rows[i].reason_href = where;
			}
			rows[i].facts = facts;
			rows[i].quality = quality_words(f);
			rows[i].size = f->n_files > 0 ? f->size_bytes : -1;
			rows[i].added_us = f->added_us;
		}
		ADD(content, axis_web_titles_new(work->view, "Movie", "Quality", rows, n));
		for (i = 0; i < n; i++)
			axis_web_title_row_clear(&rows[i]);
		g_free(rows);
	}
	ADD(content, axis_web_list_foot_new(&address, work->page.matching, n, "movie", "movies", work->page.next,
		work->page.previous));
	ADD(content, axis_web_storage_text("help", list_note));
	return axis_web_page_respond(request, "All movies", HTMX_NODE(content));
}

/* A query value as it may be used: valid UTF-8 of at most @max bytes and
 * not empty, else %NULL. */
static gchar *
query_value(
	HtmxRequest *request,
	const gchar *name,
	gsize        max
){
	const gchar *value = htmx_request_get_query_param(request, name);
	g_autofree gchar *stripped = NULL;

	if (value == NULL || strlen(value) > max || !g_utf8_validate(value, -1, NULL))
		return NULL;
	stripped = g_strstrip(g_strdup(value));
	return stripped[0] != '\0' ? g_steal_pointer(&stripped) : NULL;
}

static HtmxResponse *
handle_list(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	ListWork *work = g_new0(ListWork, 1);
	const AxisAuthState *auth = axis_web_auth_state(request);
	g_autofree gchar *state = query_value(request, "state", 32);
	g_autofree gchar *sort = query_value(request, "sort", 16);
	g_autofree gchar *view = query_value(request, "view", 16);
	const gchar *done = htmx_request_get_query_param(request, "done");
	AxisMovieState parsed;
	AxisMoviesSort sorted;
	AxisWebView viewed;

	work->user_id = auth != NULL ? auth->user_id : 0;
	/* What does not name a state, an order or a view is left out. */
	if (state != NULL && axis_movie_state_from_name(state, &parsed))
		work->state = g_steal_pointer(&state);
	if (sort != NULL && axis_movies_sort_from_name(sort, &sorted))
		work->sort = g_steal_pointer(&sort);
	if (view != NULL && axis_web_view_from_name(view, &viewed))
		work->view_asked = g_steal_pointer(&view);
	work->name = query_value(request, "q", 300);
	work->after = query_value(request, "after", 2000);
	work->before = query_value(request, "before", 2000);
	if (axis_movies_web_done_message(done) != NULL)
		work->done = g_strdup(done);
	{
		AxisWeb *web = axis_web_from_request(request);
		AxisModuleRegistry *registry = web != NULL ? axis_web_get_registry(web) : NULL;

		work->identify_on = registry != NULL && axis_module_registry_is_enabled(registry, AXIS_IDENTIFY_MODULE_ID);
	}
	return axis_movies_web_dispatch(user_data, request, list_run, list_done, work, list_work_free);
}

/* ── Wanted ─────────────────────────────────────────────────────── */

static const AxisWebColumn wanted_columns[] = {
	{ "Movie", AXIS_WEB_COL_NAME },
	{ "State", AXIS_WEB_COL_STATE },
	{ "Why", AXIS_WEB_COL_WIDE },
	{ "Last searched", AXIS_WEB_COL_DATE },
	{ "Next search", AXIS_WEB_COL_DATE },
	{ NULL, 0 }
};

static void
wanted_run(
	AxisMovies *self,
	gpointer    data
){
	GridWork *work = data;
	g_autoptr(GPtrArray) films = NULL;
	AxisMovieRules rules;

	if (!axis_movies_list(self, &films, &work->error))
		return;
	axis_movies_get_rules(self, &rules);
	work->films = g_ptr_array_new_with_free_func((GDestroyNotify)axis_movies_shown_free);
	while (films->len > 0) {
		AxisMoviesShown *shown = axis_movies_shown_new(g_ptr_array_steal_index(films, 0), &rules);
		gboolean wanted = shown->state == AXIS_MOVIE_WANTED || shown->state == AXIS_MOVIE_UPGRADE_WANTED ||
			shown->state == AXIS_MOVIE_ATTENTION || shown->state == AXIS_MOVIE_SEARCHING;

		if (wanted)
			work->counts[shown->state]++;
		if (!wanted || (work->filtered && shown->state != work->filter)) {
			axis_movies_shown_free(shown);
			continue;
		}
		g_ptr_array_add(work->films, shown);
	}
}

static HtmxResponse *
wanted_done(
	AxisMovies  *self,
	HtmxRequest *request,
	gpointer     data
){
	GridWork *work = data;
	g_autoptr(HtmxDiv) content = htmx_div_new();
	HtmxNav *tabs;
	HtmxUl *list;
	guint total;
	guint i;

	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	total = work->counts[AXIS_MOVIE_WANTED] + work->counts[AXIS_MOVIE_UPGRADE_WANTED] +
		work->counts[AXIS_MOVIE_ATTENTION] + work->counts[AXIS_MOVIE_SEARCHING];
	ADD(content, heading(1, "Wanted movies"));
	ADD(content, axis_web_storage_text(NULL, "The movies Axis is looking for, and those that need a decision. Each "
		"is searched for automatically, the least recently searched first, with more time between searches while "
		"nothing is found."));
	tabs = htmx_nav_new();
	htmx_element_add_class(HTMX_ELEMENT(tabs), "kind-tabs");
	htmx_element_set_attribute(HTMX_ELEMENT(tabs), "aria-label", "Wanted movies");
	list = htmx_ul_new();
	ADD(list, tab_item("/films/wanted", "All", total, !work->filtered));
	ADD(list, tab_item("/films/wanted?state=attention", "Needs attention", work->counts[AXIS_MOVIE_ATTENTION],
		work->filtered && work->filter == AXIS_MOVIE_ATTENTION));
	ADD(tabs, list);
	ADD(content, tabs);
	if (work->films->len == 0) {
		ADD(content, axis_web_storage_text("no-results", work->filtered ? "No movie needs attention." :
			"No movie is wanted."));
	} else {
		HtmxNode *card = axis_web_storage_card(work->filtered ? "Movies that need attention" : "All wanted movies");
		HtmxNode *table = axis_web_table_new("films-wanted", wanted_columns);
		HtmxTbody *body = htmx_tbody_new();

		for (i = 0; i < work->films->len; i++) {
			const AxisMoviesShown *shown = g_ptr_array_index(work->films, i);
			const AxisMovieFacts *f = shown->facts;
			g_autofree gchar *href = g_strdup_printf("/title/%" G_GINT64_FORMAT, f->title_id);
			g_autofree gchar *label = axis_movies_title_label(f->title, f->year);
			HtmxTr *row = htmx_tr_new();
			HtmxA *link = htmx_a_new_with_href(href);
			HtmxBdi *name = htmx_bdi_new();

			htmx_node_set_text_content(HTMX_NODE(name), label);
			ADD(link, name);
			ADD(row, cell_node(HTMX_NODE(link)));
			ADD(row, cell_node(mark_of(shown)));
			if (shown->state == AXIS_MOVIE_ATTENTION) {
				/* What is wrong, a link to where it is put right. */
				g_autofree gchar *where = attention_href(shown);
				HtmxA *why = htmx_a_new_with_href(where);

				htmx_node_set_text_content(HTMX_NODE(why), shown->reason != NULL ? shown->reason : "");
				ADD(row, cell_node(HTMX_NODE(why)));
			} else {
				ADD(row, cell_text(shown->reason));
			}
			ADD(row, cell_time(f->last_search_us));
			if (f->monitored)
				ADD(row, f->next_search_us > 0 ? cell_time(f->next_search_us) : cell_text("With the next automatic search"));
			else
				ADD(row, cell_text("Not monitored"));
			ADD(body, row);
		}
		ADD(table, body);
		ADD(card, table);
		ADD(content, card);
	}
	return axis_web_page_respond(request, "Wanted movies", HTMX_NODE(content));
}

static HtmxResponse *
handle_wanted(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	GridWork *work = g_new0(GridWork, 1);

	work->filtered = g_strcmp0(htmx_request_get_query_param(request, "state"), "attention") == 0;
	work->filter = AXIS_MOVIE_ATTENTION;
	return axis_movies_web_dispatch(user_data, request, wanted_run, wanted_done, work, grid_work_free);
}

/* ── History ────────────────────────────────────────────────────── */

typedef struct
{
	gint64     before;
	GPtrArray *events;
	GError    *error;
} HistoryWork;

static void
history_work_free(gpointer data)
{
	HistoryWork *work = data;

	g_clear_pointer(&work->events, g_ptr_array_unref);
	g_clear_error(&work->error);
	g_free(work);
}

static void
history_run(
	AxisMovies *self,
	gpointer    data
){
	HistoryWork *work = data;

	axis_movies_list_events(self, work->before, AXIS_MOVIES_WEB_HISTORY_PAGE + 1, &work->events, &work->error);
}

static const AxisWebColumn history_columns[] = {
	{ "When", AXIS_WEB_COL_DATE },
	{ "Movie", AXIS_WEB_COL_NAME },
	{ "What", AXIS_WEB_COL_TEXT },
	{ "Details", AXIS_WEB_COL_WIDE },
	{ "By", AXIS_WEB_COL_TEXT },
	{ NULL, 0 }
};

static HtmxResponse *
history_done(
	AxisMovies  *self,
	HtmxRequest *request,
	gpointer     data
){
	HistoryWork *work = data;
	g_autoptr(HtmxDiv) content = htmx_div_new();
	guint shown;
	guint i;

	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	ADD(content, heading(1, "Movie history"));
	ADD(content, axis_web_storage_text(NULL, "What happened to every movie: searches and what they found, grabs and "
		"why each release was chosen, failures, the blocklist, arrivals, and every administrator's action."));
	shown = MIN(work->events->len, AXIS_MOVIES_WEB_HISTORY_PAGE);
	if (shown == 0) {
		ADD(content, axis_web_storage_text("no-results", work->before > 0 ? "Nothing older." : "No movie has a history "
			"yet. Add a movie, and its searches, downloads and imports are listed here."));
	} else {
		HtmxNode *card = axis_web_storage_card("Events");
		HtmxNode *table = axis_web_table_new("films-history", history_columns);
		HtmxTbody *body = htmx_tbody_new();

		for (i = 0; i < shown; i++) {
			const AxisMovieEvent *e = g_ptr_array_index(work->events, i);
			HtmxTr *row = htmx_tr_new();
			HtmxBdi *name = htmx_bdi_new();

			ADD(row, cell_time(e->created_us));
			htmx_node_set_text_content(HTMX_NODE(name), e->title_label);
			if (e->title_id > 0) {
				g_autofree gchar *href = g_strdup_printf("/title/%" G_GINT64_FORMAT, e->title_id);
				HtmxA *link = htmx_a_new_with_href(href);

				ADD(link, name);
				ADD(row, cell_node(HTMX_NODE(link)));
			} else {
				ADD(row, cell_node(HTMX_NODE(name)));
			}
			ADD(row, cell_text(event_label(e->kind)));
			ADD(row, cell_breakable(e->text));
			ADD(row, cell_text(e->by_user != NULL ? e->by_user : "Axis"));
			ADD(body, row);
		}
		ADD(table, body);
		ADD(card, table);
		if (work->events->len > AXIS_MOVIES_WEB_HISTORY_PAGE) {
			const AxisMovieEvent *last = g_ptr_array_index(work->events, shown - 1);
			g_autofree gchar *href = g_strdup_printf("/films/history?before=%" G_GINT64_FORMAT, last->id);
			HtmxDiv *pager = htmx_div_new();

			htmx_element_add_class(HTMX_ELEMENT(pager), "pager");
			ADD(pager, axis_web_browser_link(href, "Older movie events"));
			ADD(card, pager);
		}
		ADD(content, card);
	}
	return axis_web_page_respond(request, "Movie history", HTMX_NODE(content));
}

static HtmxResponse *
handle_history(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	HistoryWork *work = g_new0(HistoryWork, 1);
	const gchar *before = htmx_request_get_query_param(request, "before");
	guint64 value = 0;

	if (before != NULL && g_ascii_string_to_unsigned(before, 10, 1, G_MAXINT64, &value, NULL))
		work->before = (gint64)value;
	return axis_movies_web_dispatch(user_data, request, history_run, history_done, work, history_work_free);
}

void
axis_movies_web_register_pages(
	AxisMovies *self,
	AxisWeb    *web
){
	axis_web_route_full(web, HTMX_METHOD_GET, "/films", AXIS_WEB_ROUTE_DEFAULT, handle_list, self);
	axis_web_route_full(web, HTMX_METHOD_GET, "/films/wanted", AXIS_WEB_ROUTE_DEFAULT, handle_wanted, self);
	axis_web_route_full(web, HTMX_METHOD_GET, "/films/history", AXIS_WEB_ROUTE_DEFAULT, handle_history, self);
}
