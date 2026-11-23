/*
 * axis-web-indexers-pages.c - the Indexers pages: the native indexers and
 * the sources in one list, one indexer's or source's state, what it can
 * search by, test searches and settings
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Administrators only.  A key is never put back in a page: its field is a
 * password field without a value, typed into only to replace the stored
 * key, and what was typed is wiped once it has been used.  A refused form is
 * shown again with what was typed (but never the key) and the layer's
 * reason, which is fixed text naming the field, never its value.  Messages
 * after a redirect come from fixed keys.
 *
 * The test searches show raw release names as text (the layer returns them
 * unparsed; nothing here calls the release parser), never a release's fetch
 * reference or magnet link (the pages never receive them), and are logged
 * as a test search and nothing more.  An origin of files a test saw is
 * shown as its scheme, host and port, which hold no key; accepting it takes
 * the stored one, never one from the form.
 */

#include "web/axis-web-indexers-private.h"

#include <string.h>

#include "indexer/axis-indexer-cardigann.h"
#include "indexer/axis-indexer-newznab.h"
#include "indexer/axis-indexer-prowlarr.h"
#include "indexer/axis-indexer-sites.h"
#include "web/axis-password.h"
#include "web/axis-web-auth-private.h"
#include "web/axis-web-browser-private.h"
#include "web/axis-web-notice.h"
#include "web/axis-web-place.h"
#include "web/axis-web-setup-path.h"
#include "web/axis-web-storage-private.h"

#define ADD axis_web_storage_add

/* ── Form values ────────────────────────────────────────────────── */

/* What a form held, shown again when it is refused (never the key). */
typedef struct
{
	gchar    *name;
	gchar    *kind;
	gchar    *address;
	gchar    *base_path;
	gboolean  accept_untrusted;
	gboolean  enabled;
	gboolean  follow_redirects;
	gchar    *priority;
	gchar    *movie_categories;
	gchar    *tv_categories;
	gchar    *files_origin;
	gchar    *definition;
} Values;

static void
values_clear(Values *values)
{
	g_clear_pointer(&values->name, g_free);
	g_clear_pointer(&values->kind, g_free);
	g_clear_pointer(&values->address, g_free);
	g_clear_pointer(&values->base_path, g_free);
	g_clear_pointer(&values->priority, g_free);
	g_clear_pointer(&values->movie_categories, g_free);
	g_clear_pointer(&values->tv_categories, g_free);
	g_clear_pointer(&values->files_origin, g_free);
	g_clear_pointer(&values->definition, g_free);
}

/* "2000, 2040" */
static gchar *
categories_text(
	const gint *values,
	guint       n
){
	GString *text = g_string_new(NULL);
	guint i;

	for (i = 0; i < n; i++)
		g_string_append_printf(text, "%s%d", i > 0 ? ", " : "", values[i]);
	return g_string_free(text, FALSE);
}

static void
values_from_source(
	Values                      *values,
	const AxisIndexerSourceInfo *source
){
	memset(values, 0, sizeof(*values));
	values->name = g_strdup(source->name);
	values->kind = g_strdup(source->kind);
	/* One that runs a definition has its path in its address. */
	values->address = source->definition_id != 0 ? g_strconcat(source->address, source->base_path, NULL) :
		g_strdup(source->address);
	values->base_path = g_strdup(source->base_path);
	values->accept_untrusted = source->accept_untrusted;
	values->enabled = source->enabled;
	values->follow_redirects = source->follow_redirects;
	values->priority = g_strdup_printf("%d", source->priority);
	values->movie_categories = categories_text(source->movie_categories, source->n_movie_categories);
	values->tv_categories = categories_text(source->tv_categories, source->n_tv_categories);
	values->files_origin = g_strdup(source->files_origin);
}

/* A new indexer's form: its kind's own address when it has one (a public
 * site's is shown, to change or keep). */
static void
values_fresh(
	Values                    *values,
	const gchar               *kind_name,
	const AxisIndexerKindInfo *kind
){
	memset(values, 0, sizeof(*values));
	values->kind = g_strdup(kind_name);
	values->enabled = TRUE;
	values->follow_redirects = TRUE;
	if (kind != NULL && kind->default_address != NULL)
		values->address = g_strdup(kind->default_address);
}

/* A form field, kept only when it is short, valid text. */
static gchar *
form_text(
	HtmxRequest *request,
	const gchar *name,
	gsize        max_bytes
){
	const gchar *value = htmx_request_get_form_value(request, name);

	if (value == NULL || strlen(value) > max_bytes || !g_utf8_validate(value, -1, NULL))
		return NULL;
	return g_strdup(value);
}

static void
values_from_form(
	Values      *values,
	HtmxRequest *request
){
	memset(values, 0, sizeof(*values));
	values->name = form_text(request, "name", 256);
	values->kind = form_text(request, "kind", 32);
	values->address = form_text(request, "address", 512);
	/* An address typed beside a definition's links wins over them. */
	{
		g_autofree gchar *other = form_text(request, "address_other", 512);

		if (other != NULL && g_strstrip(other)[0] != '\0') {
			g_free(values->address);
			values->address = g_steal_pointer(&other);
		}
	}
	values->definition = form_text(request, "definition", 20);
	values->base_path = form_text(request, "base_path", 256);
	values->accept_untrusted = axis_web_storage_form_yes(request, "accept_untrusted");
	values->enabled = axis_web_storage_form_yes(request, "enabled");
	values->follow_redirects = axis_web_storage_form_yes(request, "follow_redirects");
	values->priority = form_text(request, "priority", 16);
	values->movie_categories = form_text(request, "movie_categories", 200);
	values->tv_categories = form_text(request, "tv_categories", 200);
	values->files_origin = form_text(request, "files_origin", 512);
}

/* What the test search form held. */
typedef struct
{
	gchar *kind;
	gchar *title;
	gchar *year;
	gchar *imdb;
	gchar *tmdb;
	gchar *tvdb;
	gchar *season;
	gchar *episode;
} Query;

static void
query_clear(Query *query)
{
	g_clear_pointer(&query->kind, g_free);
	g_clear_pointer(&query->title, g_free);
	g_clear_pointer(&query->year, g_free);
	g_clear_pointer(&query->imdb, g_free);
	g_clear_pointer(&query->tmdb, g_free);
	g_clear_pointer(&query->tvdb, g_free);
	g_clear_pointer(&query->season, g_free);
	g_clear_pointer(&query->episode, g_free);
}

static void
query_from_form(
	Query       *query,
	HtmxRequest *request
){
	memset(query, 0, sizeof(*query));
	query->kind = form_text(request, "kind", 16);
	query->title = form_text(request, "title", 1024);
	query->year = form_text(request, "year", 16);
	query->imdb = form_text(request, "imdb", 32);
	query->tmdb = form_text(request, "tmdb", 16);
	query->tvdb = form_text(request, "tvdb", 16);
	query->season = form_text(request, "season", 16);
	query->episode = form_text(request, "episode", 16);
}

/* A number typed in a field: empty is @none; anything else must be digits. */
static gboolean
number_of(
	const gchar *text,
	gint64       none,
	gint64      *value
){
	g_autofree gchar *clean = text != NULL ? g_strstrip(g_strdup(text)) : g_strdup("");
	gchar *end = NULL;
	guint64 parsed;
	const gchar *p;

	if (clean[0] == '\0') {
		*value = none;
		return TRUE;
	}
	if (strlen(clean) > 10)
		return FALSE;
	for (p = clean; *p != '\0'; p++) {
		if (!g_ascii_isdigit(*p))
			return FALSE;
	}
	parsed = g_ascii_strtoull(clean, &end, 10);
	if (end == NULL || *end != '\0' || parsed > G_MAXINT32)
		return FALSE;
	*value = (gint64)parsed;
	return TRUE;
}

/* The request a test search form describes; %FALSE with @problem when its
 * numbers are not numbers. */
static gboolean
request_from_query(
	const Query         *query,
	AxisIndexerRequest  *request,
	const gchar        **problem
){
	AxisIndexerQueryKind kind = AXIS_INDEXER_QUERY_FILM;
	gint64 year;
	gint64 tmdb;
	gint64 tvdb;
	gint64 season;
	gint64 episode;

	*problem = NULL;
	if (query->kind != NULL && !axis_indexer_query_kind_from_string(query->kind, &kind)) {
		*problem = "Choose what to look for.";
		return FALSE;
	}
	if (!number_of(query->year, 0, &year) || !number_of(query->tmdb, 0, &tmdb) ||
	    !number_of(query->tvdb, 0, &tvdb) || !number_of(query->season, -1, &season) ||
	    !number_of(query->episode, -1, &episode)) {
		*problem = "The year, the identifiers, the season and the episode are whole numbers.";
		return FALSE;
	}
	axis_indexer_request_init(request, kind);
	request->year = (gint)year;
	request->tmdb_id = tmdb;
	request->tvdb_id = tvdb;
	request->season = (gint)season;
	request->episode = (gint)episode;
	return TRUE;
}

/* ── Pieces ─────────────────────────────────────────────────────── */

static HtmxNode *
heading(
	guint        level,
	const gchar *text
){
	HtmxHeading *node = level == 1 ? htmx_h1_new() : htmx_h3_new();

	htmx_node_set_text_content(HTMX_NODE(node), text);
	return HTMX_NODE(node);
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

/* A name that came from outside, as text in its own element, wrapping
 * after its separators. */
static HtmxNode *
name_node(const gchar *text)
{
	HtmxBdi *bdi = htmx_bdi_new();

	ADD(bdi, axis_web_breakable(text));
	return HTMX_NODE(bdi);
}

/* A line of detail under a name. */
static HtmxNode *
detail_line(const gchar *text)
{
	HtmxSpan *line = htmx_span_new();

	htmx_element_add_class(HTMX_ELEMENT(line), "range-detail");
	ADD(line, axis_web_breakable(text));
	return HTMX_NODE(line);
}

static HtmxNode *
select_field(
	const gchar        *id,
	const gchar        *label,
	const gchar        *name,
	const gchar *const *values,
	const gchar *const *labels,
	const gchar        *current
){
	HtmxDiv *field = htmx_div_new();
	HtmxLabel *text = htmx_label_new_for(id);
	HtmxSelect *select = htmx_select_new_with_name(name);
	guint i;

	htmx_element_add_class(HTMX_ELEMENT(field), "field");
	htmx_node_set_text_content(HTMX_NODE(text), label);
	htmx_element_set_id(HTMX_ELEMENT(select), id);
	for (i = 0; values[i] != NULL; i++) {
		HtmxOption *option = htmx_option_new_with_value(values[i], labels[i]);

		if (g_strcmp0(values[i], current) == 0)
			htmx_option_set_selected(option, TRUE);
		ADD(select, option);
	}
	ADD(field, text);
	ADD(field, select);
	return HTMX_NODE(field);
}

static HtmxNode *
checkbox(
	const gchar *id,
	const gchar *label,
	const gchar *name,
	gboolean     checked
){
	HtmxDiv *field = htmx_div_new();
	HtmxInput *box = htmx_input_new(HTMX_INPUT_CHECKBOX);
	HtmxLabel *text = htmx_label_new_for(id);

	htmx_element_add_class(HTMX_ELEMENT(field), "field-check");
	htmx_element_set_id(HTMX_ELEMENT(box), id);
	htmx_input_set_name(box, name);
	htmx_input_set_value(box, "yes");
	if (checked)
		htmx_element_set_attribute(HTMX_ELEMENT(box), "checked", "checked");
	htmx_node_set_text_content(HTMX_NODE(text), label);
	ADD(field, box);
	ADD(field, text);
	return HTMX_NODE(field);
}

/* The key's field: never a value, only the way to replace it. */
static HtmxNode *
secret_field(
	const gchar *id,
	gboolean     set,
	gboolean     native
){
	HtmxDiv *field = htmx_div_new();
	HtmxLabel *label = htmx_label_new_for(id);
	HtmxInput *input = htmx_input_new(HTMX_INPUT_PASSWORD);
	const gchar *where = native ? " It is on the indexer's site, in your profile or on its API page." : "";
	g_autofree gchar *help = g_strdup_printf("%s%s", set ?
		"A key is stored. Type a new one to replace it; leave this empty to keep it. It is never shown again, never "
		"written to a log, and sent only to this address." :
		"It is never shown again, never written to a log, and sent only to this address.", set ? "" : where);

	htmx_element_add_class(HTMX_ELEMENT(field), "field");
	htmx_node_set_text_content(HTMX_NODE(label), "API key");
	htmx_element_set_id(HTMX_ELEMENT(input), id);
	htmx_input_set_name(input, "secret");
	htmx_element_set_attribute(HTMX_ELEMENT(input), "autocomplete", "new-password");
	htmx_input_set_placeholder(input, set ? "Type a new key to replace it" : native ? "The indexer's API key" :
		"Prowlarr's API key");
	ADD(field, label);
	ADD(field, input);
	ADD(field, axis_web_storage_text("help", help));
	return HTMX_NODE(field);
}

static HtmxNode *
state_badge(AxisIndexerSourceState state)
{
	static const gchar *const css[] = { "badge-note", "badge-succeeded", "badge-failed", "badge-failed", "badge-warn" };

	return axis_web_browser_badge(axis_indexer_source_state_label(state), css[CLAMP((gint)state, 0, 4)]);
}

static HtmxNode *
plain_http_warning(gboolean has_key)
{
	return axis_web_storage_text("notice notice-warning", has_key ? "This address is plain http: the key and "
		"everything else cross the network unencrypted. That is usual inside one machine or a compose network; across "
		"a network, give it an https address." : "This address is plain http: everything crosses the network "
		"unencrypted, and whoever can see it sees what is searched for. That is usual inside one machine or a "
		"compose network; across a network, give it an https address.");
}

/* How many of a source's indexers are on, and how many of those fail. */
static void
count_indexers(
	const AxisIndexerSourceInfo *source,
	guint                       *on,
	guint                       *failing
){
	guint i;

	*on = 0;
	*failing = 0;
	for (i = 0; i < source->indexers->len; i++) {
		const AxisIndexerInfo *info = g_ptr_array_index(source->indexers, i);

		if (!info->enabled)
			continue;
		(*on)++;
		if (info->failing)
			(*failing)++;
	}
}

static const gchar *
kind_label(
	AxisWebIndexers *self,
	const gchar     *name
){
	const AxisIndexerKindInfo *kind = axis_indexers_find_kind(self->indexers, name);

	return kind != NULL ? kind->label : name;
}

/* "Newznab (Usenet)", "Torznab (torrents)", "Prowlarr" */
static gchar *
kind_text(
	AxisWebIndexers             *self,
	const AxisIndexerSourceInfo *source
){
	if (source->native)
		return g_strdup_printf("%s (%s)", kind_label(self, source->kind), source->protocol == AXIS_DOWNLOAD_USENET ?
			"Usenet" : "torrents");
	return g_strdup(kind_label(self, source->kind));
}

static gboolean
caps_known(const AxisIndexerSourceInfo *source)
{
	return source->caps.read_us > 0 && source->caps.problem == NULL;
}

/* What a native indexer is searched by: "Movies by IMDb, TV by TVDB". */
static gchar *
searches_by(
	const AxisIndexerKindInfo   *kind,
	const AxisIndexerSourceInfo *source
){
	const AxisIndexerCaps *caps = &source->caps;
	g_autofree gchar *movies = NULL;
	g_autofree gchar *tv = NULL;

	/* A public site says in its own words what it covers. */
	if (kind != NULL && kind->site && kind->covers != NULL)
		return g_strdup(kind->covers);
	if (source->caps.read_us == 0)
		return g_strdup("not read yet");
	if (!caps_known(source))
		return g_strdup("words only");
	movies = (caps->modes & AXIS_INDEXER_CAN_MOVIE) != 0 && caps->movie_ids != 0 ?
		axis_indexer_ids_label(caps->movie_ids) : g_strdup("words");
	tv = (caps->modes & AXIS_INDEXER_CAN_TV) != 0 && caps->tv_ids != 0 && caps->tv_numbers ?
		axis_indexer_ids_label(caps->tv_ids) : g_strdup("words");
	return g_strdup_printf("Movies by %s, TV by %s", movies, tv);
}

/* "12 of 5,000 calls" (and the files), or %NULL when not reported. */
static gchar *
allowance_text(const AxisIndexerAllowance *a)
{
	GString *text;

	if (a->api_max <= 0 && a->grab_max <= 0)
		return NULL;
	text = g_string_new(NULL);
	if (a->api_max > 0) {
		g_autofree gchar *used = axis_web_browser_format_count((guint64)MAX(a->api_used, 0));
		g_autofree gchar *max = axis_web_browser_format_count((guint64)a->api_max);

		g_string_append_printf(text, "%s of %s calls", used, max);
	}
	if (a->grab_max > 0) {
		g_autofree gchar *used = axis_web_browser_format_count((guint64)MAX(a->grab_used, 0));
		g_autofree gchar *max = axis_web_browser_format_count((guint64)a->grab_max);

		g_string_append_printf(text, "%s%s of %s files", text->len > 0 ? ", " : "", used, max);
	}
	return g_string_free(text, FALSE);
}

static gchar *
clock_text(gint64 us)
{
	return axis_web_format_time(us, "later");
}

/* The add and edit form's fields, in one stacked form, for @kind; @prefix
 * makes the fields' ids, @back (nullable) is the setup guide's address it
 * returns to. */
static HtmxNode *
source_form(
	const AxisIndexerKindInfo *kind,
	HtmxRequest               *request,
	const gchar               *action,
	const Values              *values,
	gboolean                   creating,
	gboolean                   key_set,
	const gchar               *button,
	const gchar               *prefix,
	const gchar               *back
){
	HtmxNode *form = axis_web_auth_post_form(request, action);
	HtmxNode *back_field = axis_web_setup_return_field(back);
	gboolean native = kind != NULL && kind->native;
	gboolean site = kind != NULL && kind->site;
	gboolean torznab = native && !site && kind->protocol == AXIS_DOWNLOAD_TORRENT;
	g_autofree gchar *site_help = NULL;
	g_autofree gchar *name_id = g_strdup_printf("%s-name", prefix);
	g_autofree gchar *address_id = g_strdup_printf("%s-address", prefix);
	g_autofree gchar *base_path_id = g_strdup_printf("%s-base-path", prefix);
	g_autofree gchar *secret_id = g_strdup_printf("%s-secret", prefix);
	g_autofree gchar *movies_id = g_strdup_printf("%s-movie-categories", prefix);
	g_autofree gchar *tv_id = g_strdup_printf("%s-tv-categories", prefix);
	g_autofree gchar *files_id = g_strdup_printf("%s-files-origin", prefix);
	g_autofree gchar *priority_id = g_strdup_printf("%s-priority", prefix);
	g_autofree gchar *untrusted_id = g_strdup_printf("%s-untrusted", prefix);
	g_autofree gchar *enabled_id = g_strdup_printf("%s-enabled", prefix);
	g_autofree gchar *follow_id = g_strdup_printf("%s-follow", prefix);

	htmx_element_add_class(HTMX_ELEMENT(form), "form");
	ADD(form, axis_web_storage_text_field(name_id, "Name", "name", values->name, site ? kind->label : !native ?
		"Prowlarr" : torznab ? "Jackett: Example Tracker" : "Example Usenet", "The name Axis shows on its pages."));
	if (creating && kind != NULL)
		ADD(form, axis_web_auth_hidden("kind", kind->name));
	if (site)
		site_help = kind->default_address != NULL ?
			g_strdup_printf("%s The address it is known at is %s.", kind->address_help, kind->default_address) :
			g_strdup(kind->address_help);
	ADD(form, axis_web_storage_text_field(address_id, "Address", "address", values->address, site ?
		kind->default_address : !native ? "http://prowlarr:9696" : torznab ? "http://jackett:9117" :
		"https://api.example.com", site ? site_help : kind != NULL && kind->address_help != NULL ? kind->address_help :
		"Its own address: http or https, its host and port. A path goes in the base path below."));
	if (!site)
		ADD(form, axis_web_storage_text_field(base_path_id, "Base path (optional)", "base_path", values->base_path,
		!native ? "/prowlarr" : torznab ? "/api/v2.0/indexers/example/results/torznab" : "", !native ?
		"Only when Prowlarr is served under a path, as it often is behind a reverse proxy: a slash and the "
		"path, such as /prowlarr. Leave it empty when Prowlarr is at the top of its address. It is typed here and "
		"nothing Prowlarr says changes it." : torznab ? "Everything in the feed's address after the host and port, "
		"without the /api that Axis adds itself. For Jackett that is /api/v2.0/indexers/NAME/results/torznab (Axis "
		"then asks .../torznab/api, which Jackett answers); for one of Prowlarr's indexers it is /NUMBER. It is typed "
		"here and nothing the feed says changes it." : "Only when the indexer's API is not at /api: a slash and the "
		"path in front of its /api, which Axis adds itself. An API at /api/v1/api is the base path /api/v1. Leave it "
		"empty for most indexers."));
	/* A public site takes no key, base path, categories or other address for
	 * its files. */
	if (!site)
		ADD(form, secret_field(secret_id, key_set, native));
	if (native && !site) {
		ADD(form, axis_web_storage_text_field(movies_id, "Newznab categories for movies", "movie_categories",
			values->movie_categories, "2000", "Newznab category numbers, separated by commas. Empty is 2000, all "
			"movies; an indexer's own, such as 2040 for HD, narrow it."));
		ADD(form, axis_web_storage_text_field(tv_id, "Newznab categories for TV", "tv_categories", values->tv_categories,
			"5000", "Empty is 5000, all TV."));
		ADD(form, axis_web_storage_text_field(files_id, "Files fetched from (optional)", "files_origin",
			values->files_origin, "https://files.example", "Only when the indexer serves its files from another "
			"address than its own: that address's scheme, host and port. Axis fetches files from this indexer's "
			"own address and from this one, nowhere else; a test shows where its files come from."));
	}
	ADD(form, axis_web_storage_text_field(priority_id, "Priority", "priority", values->priority, "25",
		"1 to 50, lower first: when two indexers found the same release, it is fetched from the one first here, "
		"and the other is kept to try next. Empty is 25."));
	ADD(form, checkbox(untrusted_id, "Accept a self-signed certificate", "accept_untrusted",
		values->accept_untrusted));
	if (!native) {
		ADD(form, checkbox(follow_id, "Follow download redirects to the indexers", "follow_redirects",
			values->follow_redirects));
		ADD(form, axis_web_storage_text("help", "On by default. An indexer with Prowlarr's Redirect option on (Usenet "
			"indexers often have it) answers a download with its own address instead of the file. Axis then fetches "
			"the release from there: once, over https, only from a public address, and sending nothing of its own, "
			"not Prowlarr's key. Off, those releases cannot be fetched, and the page says which indexers "
			"redirect."));
	}
	ADD(form, checkbox(enabled_id, "Switched on", "enabled", values->enabled));
	if (back_field != NULL)
		ADD(form, back_field);
	ADD(form, axis_web_auth_button(button, "button-primary"));
	return form;
}

HtmxNode *
axis_web_indexers_add_form(
	AxisIndexers *indexers,
	HtmxRequest  *request,
	const gchar  *kind_name,
	const gchar  *return_path
){
	const AxisIndexerKindInfo *kind = axis_indexers_find_kind(indexers, kind_name);
	g_autofree gchar *prefix = g_strdup_printf("new-%s", kind_name != NULL ? kind_name : "source");
	Values fresh;
	HtmxNode *form;

	if (kind == NULL)
		return axis_web_storage_text("help", "This version of Axis cannot add that kind.");
	values_fresh(&fresh, kind_name, kind);
	form = source_form(kind, request, "/indexers", &fresh, TRUE, FALSE, "Add the indexer", prefix, return_path);
	values_clear(&fresh);
	return form;
}

HtmxNode *
axis_web_indexers_edit_form(
	AxisIndexers                *indexers,
	HtmxRequest                 *request,
	const AxisIndexerSourceInfo *source,
	const gchar                 *return_path
){
	g_autofree gchar *action = g_strdup_printf("/indexers/%" G_GINT64_FORMAT, source->id);
	g_autofree gchar *prefix = g_strdup_printf("source-%" G_GINT64_FORMAT, source->id);
	Values current;
	HtmxNode *form;

	values_from_source(&current, source);
	form = source_form(axis_indexers_find_kind(indexers, source->kind), request, action, &current, FALSE,
		source->key_set, "Save", prefix, return_path);
	values_clear(&current);
	return form;
}

/* The test button, with a name for a screen reader when @aria is given. */
static HtmxNode *
test_form_named(
	HtmxRequest *request,
	gint64       source_id,
	const gchar *label,
	const gchar *css_class,
	const gchar *return_path,
	const gchar *aria
){
	g_autofree gchar *action = g_strdup_printf("/indexers/%" G_GINT64_FORMAT "/test", source_id);
	HtmxNode *form = axis_web_auth_post_form(request, action);
	HtmxNode *back = axis_web_setup_return_field(return_path);
	HtmxNode *button = axis_web_auth_button(label, css_class);

	htmx_element_add_class(HTMX_ELEMENT(form), "inline-form");
	if (back != NULL)
		ADD(form, back);
	if (aria != NULL)
		htmx_element_set_attribute(HTMX_ELEMENT(button), "aria-label", aria);
	ADD(form, button);
	return form;
}

HtmxNode *
axis_web_indexers_test_form(
	HtmxRequest *request,
	gint64       source_id,
	const gchar *label,
	const gchar *css_class,
	const gchar *return_path
){
	return test_form_named(request, source_id, label, css_class, return_path, NULL);
}

/* ── The list ───────────────────────────────────────────────────── */

static const AxisWebColumn source_columns[] = {
	{ "Name", AXIS_WEB_COL_NAME },
	{ "Kind", AXIS_WEB_COL_TEXT },
	{ "Searches by", AXIS_WEB_COL_TEXT },
	{ "State", AXIS_WEB_COL_STATE },
	{ "Allowance today", AXIS_WEB_COL_TEXT },
	{ "Last answer", AXIS_WEB_COL_DATE },
	{ "Problem", AXIS_WEB_COL_WIDE },
	{ "", AXIS_WEB_COL_ACTIONS },
	{ NULL, 0 }
};

/* What the list says is wrong with @source, or "" for nothing. */
static gchar *
problem_text(
	const AxisIndexerKindInfo   *kind,
	const AxisIndexerSourceInfo *source
){
	GString *why = g_string_new(NULL);
	gint64 now_us = g_get_real_time();

	if (source->state != AXIS_INDEXER_SOURCE_OK && source->state_message != NULL)
		g_string_append(why, source->state_message);
	if (!source->key_set && !(kind != NULL && kind->site))
		g_string_append_printf(why, "%sNo API key is set: add it under Settings for this indexer.",
			why->len > 0 ? " " : "");
	if (!source->native && source->enabled && (axis_indexer_source_redirects_refused(source, AXIS_DOWNLOAD_USENET) ||
	    axis_indexer_source_redirects_refused(source, AXIS_DOWNLOAD_TORRENT)))
		g_string_append_printf(why, "%sSome of its indexers redirect their downloads, and following redirects is "
			"switched off: switch on Follow download redirects to the indexers in its settings.",
			why->len > 0 ? " " : "");
	if (source->native) {
		AxisIndexerHoldCause cause = axis_indexer_hold_cause(source, now_us);

		if (cause == AXIS_INDEXER_HOLD_DRIFT || cause == AXIS_INDEXER_HOLD_CHALLENGE) {
			g_autofree gchar *until = clock_text(source->held_until_us);

			/* Axis's own hold, for what its answers were: its state says why. */
			g_string_append_printf(why, "%s%s, so searches pass it over until %s; a test or a check that passes ends "
				"that.", why->len > 0 ? " " : "", cause == AXIS_INDEXER_HOLD_DRIFT ? "Its pages no longer look as "
				"expected" : "It answers with a browser challenge", until);
		} else if (cause == AXIS_INDEXER_HOLD_WAIT) {
			g_autofree gchar *until = clock_text(source->held_until_us);

			g_string_append_printf(why, "%sThe site asked Axis to wait: it is not asked, by a test or a check either, "
				"until %s.", why->len > 0 ? " " : "", until);
		} else if (source->held_until_us > now_us) {
			g_autofree gchar *until = clock_text(source->held_until_us);

			g_string_append_printf(why, "%sIt said its request limit is reached: not asked until %s.",
				why->len > 0 ? " " : "", until);
		} else if (axis_indexer_allowance_spent(&source->allowance, FALSE)) {
			g_string_append_printf(why, "%sIts daily allowance is used up.", why->len > 0 ? " " : "");
		} else if (axis_indexer_allowance_spent(&source->allowance, TRUE)) {
			g_string_append_printf(why, "%sNear its daily allowance: automatic searches pass it over, keeping the "
				"rest for searches you ask for.", why->len > 0 ? " " : "");
		}
		if (source->grabs_held_until_us > now_us || axis_indexer_allowance_grabs_spent(&source->allowance))
			g_string_append_printf(why, "%sIts daily allowance of files is used up.", why->len > 0 ? " " : "");
		if (source->caps.read_us > 0 && source->caps.problem != NULL)
			g_string_append_printf(why, "%sWhat it can search by could not be read: it is searched by words only.",
				why->len > 0 ? " " : "");
		if (source->files_origin_seen != NULL)
			g_string_append_printf(why, "%sIt serves its files from another address: accept it on its page.",
				why->len > 0 ? " " : "");
	}
	return g_string_free(why, FALSE);
}

static HtmxNode *
source_row(
	AxisWebIndexers             *self,
	HtmxRequest                 *request,
	const AxisIndexerSourceInfo *source
){
	HtmxTr *row = htmx_tr_new();
	HtmxTd *name = htmx_td_new();
	HtmxSpan *address = htmx_span_new();
	HtmxTd *allowance = htmx_td_new();
	g_autofree gchar *href = g_strdup_printf("/indexers/%" G_GINT64_FORMAT, source->id);
	g_autofree gchar *id = g_strdup_printf("source-%" G_GINT64_FORMAT, source->id);
	g_autofree gchar *contact = axis_web_format_time(source->last_contact_us, "never");
	g_autofree gchar *shown = g_strconcat(source->address, source->base_path, NULL);
	g_autofree gchar *count = NULL;
	g_autofree gchar *kind = kind_text(self, source);
	const AxisIndexerKindInfo *info = axis_indexers_find_kind(self->indexers, source->kind);
	g_autofree gchar *why = problem_text(info, source);
	HtmxA *link = htmx_a_new_with_href(href);

	htmx_element_set_id(HTMX_ELEMENT(row), id);
	ADD(link, name_node(source->name));
	ADD(name, link);
	if (!source->enabled)
		ADD(name, axis_web_browser_badge("Off", "badge-note"));
	htmx_element_add_class(HTMX_ELEMENT(address), "range-detail");
	{
		HtmxCode *code = htmx_code_new();

		ADD(code, axis_web_breakable(shown));
		ADD(address, code);
	}
	if (source->plain_http)
		ADD(address, axis_web_browser_badge("Not encrypted", "badge-warn"));
	ADD(name, address);
	ADD(row, name);
	ADD(row, cell_text(kind));
	if (source->native) {
		count = searches_by(info, source);
	} else {
		guint on;
		guint failing;

		count_indexers(source, &on, &failing);
		if (source->indexers_us == 0)
			count = g_strdup("not read yet");
		else {
			g_autofree gchar *on_text = axis_web_format_count(on);
			g_autofree gchar *failing_text = axis_web_format_count(failing);

			count = failing > 0 ? g_strdup_printf("%s %s on, %s failing", on_text, on == 1 ? "indexer" : "indexers",
				failing_text) : g_strdup_printf("%s %s on", on_text, on == 1 ? "indexer" : "indexers");
		}
	}
	ADD(row, cell_text(count));
	ADD(row, cell_node(state_badge(source->state)));
	if (source->native) {
		g_autofree gchar *used = allowance_text(&source->allowance);
		HtmxSpan *text = htmx_span_new();
		/* One that runs a definition: held by Axis for what its answers were,
		 * or by the site, which asked Axis to wait. */
		AxisIndexerHoldCause cause = axis_indexer_hold_cause(source, g_get_real_time());

		htmx_node_set_text_content(HTMX_NODE(text), used != NULL ? used : "not reported");
		ADD(allowance, text);
		if (cause == AXIS_INDEXER_HOLD_DRIFT || cause == AXIS_INDEXER_HOLD_CHALLENGE)
			ADD(allowance, axis_web_browser_badge("Passed over", "badge-warn"));
		else if (cause == AXIS_INDEXER_HOLD_WAIT)
			ADD(allowance, axis_web_browser_badge("Asked to wait", "badge-warn"));
		else if (axis_indexer_source_spent(source, TRUE, g_get_real_time()))
			ADD(allowance, axis_web_browser_badge(axis_indexer_allowance_spent(&source->allowance, FALSE) ||
				source->held_until_us > g_get_real_time() ? "Used up" : "Near the limit", "badge-warn"));
	}
	ADD(row, allowance);
	ADD(row, cell_text(contact));
	ADD(row, cell_text(why));
	{
		HtmxTd *actions = htmx_td_new();

		g_autofree gchar *aria = g_strdup_printf("Test %s", source->name);

		ADD(actions, test_form_named(request, source->id, "Test", "button-small", NULL, aria));
		/* One that runs a definition can be checked step by step. */
		if (source->definition_id != 0) {
			g_autofree gchar *check = g_strdup_printf("/indexers/%" G_GINT64_FORMAT "/check", source->id);
			HtmxA *check_link = htmx_a_new_with_href(check);

			htmx_element_add_class(HTMX_ELEMENT(check_link), "button-small");
			htmx_node_set_text_content(HTMX_NODE(check_link), "Check");
			ADD(actions, check_link);
		}
		ADD(row, actions);
	}
	return HTMX_NODE(row);
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
		{ "added", FALSE, "The indexer has been added. Test it now: an indexer's test reads what it can search by and where "
		  "its files come from; Prowlarr's reads its indexers." },
		{ "saved", FALSE, "The settings have been saved." },
		{ "tested", FALSE, "The indexer has been tested: the result is under State of this indexer." },
		{ "accepted", FALSE, "The address of its files has been accepted: its releases there can be fetched now. "
		  "Search again to see them." },
		{ "deleted", FALSE, "The indexer has been removed, with the search results kept for it." },
		{ "definition-removed", FALSE, "The definition has been removed." },
		{ "jackett-checked", FALSE, "Axis has read the list of Jackett's definition files again. A definition whose "
		  "file changed there is marked Newer at Jackett; nothing was updated." },
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

/* The test search's form, with what was typed. */
static HtmxNode *
search_form(
	HtmxRequest *request,
	const gchar *action,
	const Query *query,
	const gchar *button_label
){
	static const gchar *const kinds[] = { "film", "episode", "season", "text", NULL };
	static const gchar *const kind_labels[] = { "A movie", "An episode", "A season", "Words as typed", NULL };
	HtmxNode *form = axis_web_auth_post_form(request, action);
	HtmxNode *title = axis_web_storage_text_field("search-title", "Title or words", "title", query->title, "Hearth",
		NULL);
	HtmxDiv *buttons = htmx_div_new();

	HtmxDetails *more = htmx_details_new();
	HtmxSummary *head = htmx_summary_new();
	HtmxDiv *fields = htmx_div_new();
	const gchar *typed[6];
	guint i;

	/* The filter row of the other pages: the controls share one height and
	 * one bottom edge, and wrap as whole fields.  The fields' help is in
	 * the card's text above them.  What depends on the choice of "Look for"
	 * (the select is in this same form, and nothing of ours runs in the
	 * page) sits in a closed group under the row: the person types the
	 * title or words and chooses what to look for, and opens the rest only
	 * to add identifiers or an episode. */
	typed[0] = query->year;
	typed[1] = query->imdb;
	typed[2] = query->tmdb;
	typed[3] = query->tvdb;
	typed[4] = query->season;
	typed[5] = query->episode;
	htmx_element_add_class(HTMX_ELEMENT(form), "filters");
	htmx_element_add_class(HTMX_ELEMENT(title), "field-wide");
	ADD(form, select_field("search-kind", "Look for", "kind", kinds, kind_labels,
		query->kind != NULL ? query->kind : "film"));
	ADD(form, title);
	ADD(buttons, axis_web_auth_button(button_label, "button-primary"));
	ADD(form, buttons);

	htmx_element_add_class(HTMX_ELEMENT(more), "guide-more");
	htmx_node_set_text_content(HTMX_NODE(head), "Identifiers and episode (optional)");
	ADD(more, head);
	htmx_element_add_class(HTMX_ELEMENT(fields), "filters");
	ADD(fields, axis_web_storage_text_field("search-year", "Year (for a movie)", "year", query->year, "1995", NULL));
	ADD(fields, axis_web_storage_text_field("search-imdb", "IMDb identifier", "imdb", query->imdb, "tt0600001", NULL));
	ADD(fields, axis_web_storage_text_field("search-tmdb", "TMDb identifier", "tmdb", query->tmdb, "6001", NULL));
	ADD(fields, axis_web_storage_text_field("search-tvdb", "TVDB identifier", "tvdb", query->tvdb, "82002", NULL));
	ADD(fields, axis_web_storage_text_field("search-season", "Season (for a season or an episode)", "season", query->season, "1", NULL));
	ADD(fields, axis_web_storage_text_field("search-episode", "Episode (for an episode)", "episode", query->episode, "2",
		NULL));
	ADD(more, fields);
	/* Open again when something was typed in it, so a refused or repeated
	 * search shows what it asked. */
	for (i = 0; i < G_N_ELEMENTS(typed); i++) {
		if (typed[i] != NULL && typed[i][0] != '\0')
			htmx_element_set_attribute(HTMX_ELEMENT(more), "open", "open");
	}
	ADD(form, more);
	return form;
}

static const AxisWebColumn release_columns[] = {
	{ "Release", AXIS_WEB_COL_NAME },
	{ "Indexer", AXIS_WEB_COL_TEXT },
	{ "Protocol", AXIS_WEB_COL_TEXT },
	{ "Size", AXIS_WEB_COL_SIZE },
	{ "Seeders / peers", AXIS_WEB_COL_NUMBER },
	{ "Published", AXIS_WEB_COL_DATE },
	{ NULL, 0 }
};

static const AxisWebColumn part_columns[] = {
	{ "Indexer", AXIS_WEB_COL_NAME },
	{ "Searched by", AXIS_WEB_COL_TEXT },
	{ "Releases found", AXIS_WEB_COL_COUNT },
	{ "What happened", AXIS_WEB_COL_WIDE },
	{ NULL, 0 }
};

/* One sentence about what a search found and left out. */
static HtmxNode *
result_summary(
	const AxisIndexerSearchResult *result,
	gboolean                       all
){
	GString *text = g_string_new(NULL);
	g_autofree gchar *kept_until = axis_web_format_time(result->expires_us, "");
	g_autofree gchar *found = axis_web_count(result->releases->len, "release", "releases");
	HtmxNode *node;

	if (all)
		g_string_append_printf(text, "%s found.", found);
	else if (result->not_asked != NULL)
		g_string_append_printf(text, "%s found. Not asked: %s.", found, result->not_asked);
	else
		g_string_append_printf(text, "%s found, searched by %s%s%s.", found,
			axis_indexer_mode_label(result->mode), result->also_text ?
			", and by the title for the indexers that take no identifier" : "", result->fell_back ?
			" (the identifier found nothing, so the title was used)" : "");
	if (result->n_merged > 0) {
		g_autofree gchar *merged = axis_web_count(result->n_merged, "release was", "releases were");

		g_string_append_printf(text, " %s found at another indexer as well and %s listed once: the other is "
			"tried if the first cannot give it.", merged, result->n_merged == 1 ? "is" : "are");
	}
	if (result->asked_directly != NULL) {
		guint k;

		for (k = 0; k < result->asked_directly->len; k++)
			g_string_append_printf(text, " %s.", (const gchar *)g_ptr_array_index(result->asked_directly, k));
	}
	if (result->cached)
		g_string_append_printf(text, " This is the answer of an earlier search, kept until %s; %s not asked again.",
			kept_until, all ? "they were" : "the indexer was");
	if (result->failed != NULL) {
		g_autofree gchar *total = result->n_indexers >= 0 ?
			axis_web_count((guint)result->n_indexers, "indexer", "indexers") : NULL;
		g_autofree gchar *silent = axis_web_count(g_strv_length(result->failed), "indexer", "indexers");
		g_autofree gchar *n_failed = axis_web_format_count(g_strv_length(result->failed));
		guint i;

		if (total != NULL)
			g_string_append_printf(text, " %s of %s did not answer", n_failed, total);
		else
			g_string_append_printf(text, " %s did not answer", silent);
		g_string_append(text, ": ");
		for (i = 0; result->failed[i] != NULL; i++)
			g_string_append_printf(text, "%s%s", i > 0 ? ", " : "", result->failed[i]);
		g_string_append_c(text, '.');
	}
	if (result->n_off_origin > 0) {
		g_autofree gchar *off = axis_web_count(result->n_off_origin, "result was", "results were");

		g_string_append_printf(text, " %s left out because %s named a download address that is not %s own.", off,
			result->n_off_origin == 1 ? "it" : "they", all ? "its indexer's" : "this indexer's");
	}
	if (result->n_off_origin > 0 && result->releases->len == 0)
		g_string_append(text, all ? " When that is every result of an indexer, its address is not the one it writes "
			"in its download links, or it serves its files from another address: test it to see." :
			" When that is every result, the address and base path set here are not the ones written in its "
			"download links (a reverse proxy, https), or its files come from another address: test it to see.");
	if (result->n_unusable > 0) {
		g_autofree gchar *n_unusable = axis_web_format_count(result->n_unusable);

		g_string_append_printf(text, " %s %s left out because there is no way to fetch %s.", n_unusable,
			result->n_unusable == 1 ? "result" : "results", result->n_unusable == 1 ? "it" : "them");
	}
	if (result->n_skipped > 0) {
		g_autofree gchar *n_skipped = axis_web_format_count(result->n_skipped);

		g_string_append_printf(text, " Results that could not be read: %s.", n_skipped);
	}
	if (result->truncated) {
		g_autofree gchar *cap = axis_web_format_count(AXIS_INDEXER_MAX_RESULTS);

		g_string_append_printf(text, " There were more results than were read or kept (at most %s are kept); only "
			"the first are listed.", cap);
	}
	node = axis_web_storage_text(NULL, text->str);
	g_string_free(text, TRUE);
	return node;
}

/* What each indexer and source of a search of them all did. */
static HtmxNode *
parts_table(const AxisIndexerSearchResult *result)
{
	HtmxNode *table = axis_web_table_new("indexer-parts", part_columns);
	HtmxTbody *body = htmx_tbody_new();
	guint i;

	for (i = 0; i < result->parts->len; i++) {
		const AxisIndexerSearchPart *part = g_ptr_array_index(result->parts, i);
		HtmxTr *row = htmx_tr_new();
		HtmxTd *name = htmx_td_new();
		g_autofree gchar *count = part->asked && part->problem == NULL ? axis_web_format_count(part->n_releases) :
			g_strdup("");
		const gchar *why_not = part->not_asked != NULL ? part->not_asked : part->problem != NULL ? part->problem :
			"passed over";
		g_autofree gchar *what = NULL;

		ADD(name, name_node(part->name));
		ADD(row, name);
		ADD(row, cell_text(part->asked && part->problem == NULL ? axis_indexer_mode_label(part->mode) : ""));
		ADD(row, cell_text(count));
		if (!part->asked)
			what = g_strdup_printf("Not asked: %s.", why_not);
		else if (part->problem != NULL)
			what = g_strdup_printf("No answer: %s.", part->problem);
		else
			what = g_strdup_printf("Answered%s%s.", part->cached ? ", from a search a moment ago" : "",
				part->fell_back ? " (the identifier found nothing, so the title was used)" : part->also_text ?
				" (and by the title, for its indexers that take no identifier)" : "");
		ADD(row, cell_text(what));
		ADD(body, row);
	}
	ADD(table, body);
	return table;
}

/* Whether the source @source_id (of @sources, or @source) says the indexer
 * @id redirects its downloads; and whether following is on. */
static gboolean
indexer_redirects(
	const AxisIndexerSourceInfo *source,
	gint64                       id
){
	guint i;

	if (source == NULL || source->native)
		return FALSE;
	for (i = 0; source->indexers != NULL && i < source->indexers->len; i++) {
		const AxisIndexerInfo *info = g_ptr_array_index(source->indexers, i);

		if (info->id == id)
			return info->redirect;
	}
	return FALSE;
}

static const AxisIndexerSourceInfo *
source_of(
	GPtrArray *sources,
	gint64     id
){
	guint i;

	for (i = 0; sources != NULL && i < sources->len; i++) {
		const AxisIndexerSourceInfo *source = g_ptr_array_index(sources, i);

		if (source->id == id)
			return source;
	}
	return NULL;
}

/* @sources: every one, for a search of them all; @source: the one searched. */
static HtmxNode *
results_card(
	const AxisIndexerSearchResult *result,
	const AxisIndexerSourceInfo   *source,
	GPtrArray                     *sources
){
	HtmxNode *card = axis_web_storage_card("Releases found");
	gboolean all = source == NULL;
	guint redirected = 0;
	gboolean follow = TRUE;
	guint i;

	ADD(card, result_summary(result, all));
	if (all && result->parts != NULL && result->parts->len > 0)
		ADD(card, parts_table(result));
	if (result->releases->len == 0)
		return card;
	{
		HtmxNode *table = axis_web_table_new("indexer-releases", release_columns);
		HtmxTbody *body = htmx_tbody_new();

		for (i = 0; i < result->releases->len && i < AXIS_WEB_INDEXERS_SHOWN; i++) {
			const AxisIndexerRelease *release = g_ptr_array_index(result->releases, i);
			const AxisIndexerSourceInfo *from = source != NULL ? source : source_of(sources, release->source_id);
			HtmxTr *row = htmx_tr_new();
			HtmxTd *name = htmx_td_new();
			HtmxTd *indexer = htmx_td_new();
			g_autofree gchar *size = release->size_bytes >= 0 ? axis_size_format((guint64)release->size_bytes) :
				g_strdup("");
			g_autofree gchar *peers = release->seeders >= 0 ? g_strdup_printf("%d / %d", release->seeders,
				release->leechers >= 0 ? release->leechers : 0) : g_strdup("");
			g_autofree gchar *when = axis_web_format_time(release->published_us, "");

			ADD(name, name_node(release->name));
			if (release->flags != NULL) {
				g_autofree gchar *flags = g_strjoinv(", ", release->flags);
				HtmxSpan *detail = htmx_span_new();

				htmx_element_add_class(HTMX_ELEMENT(detail), "range-detail");
				htmx_node_set_text_content(HTMX_NODE(detail), flags);
				ADD(name, detail);
			}
			/* A release its indexer's site serves, through a redirect. */
			if (indexer_redirects(from, release->indexer_id)) {
				HtmxSpan *line = htmx_span_new();

				redirected++;
				follow = follow && from->follow_redirects;
				htmx_element_add_class(HTMX_ELEMENT(line), "range-detail");
				ADD(line, axis_web_browser_badge(from->follow_redirects ? "From the indexer's site" :
					"Cannot be fetched", from->follow_redirects ? "badge-note" : "badge-warn"));
				ADD(name, line);
			}
			ADD(row, name);
			/* Which indexer it came from, and which others have it. */
			ADD(indexer, name_node(release->indexer_name != NULL ? release->indexer_name :
				from != NULL ? from->name : ""));
			if (all && from != NULL && !from->native && release->indexer_name != NULL &&
			    g_strcmp0(release->indexer_name, from->name) != 0) {
				g_autofree gchar *through = g_strdup_printf("through %s", from->name);

				ADD(indexer, detail_line(through));
			}
			if (release->also_at != NULL) {
				g_autofree gchar *names = g_strjoinv(", ", release->also_at);
				g_autofree gchar *also = g_strdup_printf("also at %s", names);

				ADD(indexer, detail_line(also));
			}
			ADD(row, indexer);
			ADD(row, cell_text(axis_download_protocol_label(release->protocol)));
			ADD(row, cell_text(size));
			ADD(row, cell_text(peers));
			ADD(row, cell_text(when));
			ADD(body, row);
		}
		ADD(table, body);
		ADD(card, table);
	}
	if (result->releases->len > AXIS_WEB_INDEXERS_SHOWN) {
		g_autofree gchar *shown = axis_web_format_count(AXIS_WEB_INDEXERS_SHOWN);
		g_autofree gchar *found = axis_web_format_count(result->releases->len);
		g_autofree gchar *more = g_strdup_printf("Showing the first %s of %s releases.", shown, found);

		ADD(card, axis_web_storage_text("help", more));
	}
	if (redirected > 0)
		ADD(card, axis_web_storage_text("help", follow ?
			"\"From the indexer's site\": its indexer redirects downloads, and Axis fetches it from there by following "
			"that one redirect." :
			"\"Cannot be fetched\": its indexer redirects downloads, and following redirects is switched off for the "
			"Prowlarr it comes through. Switch on Follow download redirects to the indexers in that Prowlarr's "
			"settings."));
	return card;
}

/* The intro of the list. */
static void
list_intro(HtmxDiv *content)
{
	ADD(content, heading(1, "Indexers"));
	ADD(content, axis_web_page_intro("Where Axis looks for releases of a movie or an episode to download: your "
		"Usenet indexers and torrent feeds, which Axis asks itself with your own keys, public torrent sites (YTS, "
		"EZTV, The Pirate Bay, TorrentProject, LimeTorrents, TheRARBG, Torrents-CSV, and others through site "
		"definitions you install), which need none, and Prowlarr if you use it, which searches every indexer "
		"configured in it. Nothing can be found to download until one is set up here."));
	ADD(content, axis_web_storage_text(NULL, "A search asks every one that is switched on at once, each within its "
		"own limits, so one that is slow or down holds up nothing else. A release found at several is listed once "
		"and fetched from the next if the first cannot give it. Axis fetches a release only from an indexer that "
		"found it, never from an address in a result, and keeps what a search found for a few minutes so that "
		"asking again costs nothing."));
}

/* One of the add blocks: a <details> with what to know and the form. */
static HtmxNode *
add_block(
	AxisWebIndexers *self,
	HtmxRequest     *request,
	const gchar     *kind_name,
	const gchar     *summary,
	const gchar     *text,
	const Values    *submitted,
	gboolean         open,
	const gchar     *back
){
	const AxisIndexerKindInfo *kind = axis_indexers_find_kind(self->indexers, kind_name);
	HtmxDetails *details = htmx_details_new();
	HtmxSummary *head = htmx_summary_new();
	g_autofree gchar *prefix = g_strdup_printf("new-%s", kind_name);
	Values fresh;

	if (kind == NULL) {
		g_object_unref(head);
		g_object_unref(details);
		return NULL;
	}
	htmx_element_add_class(HTMX_ELEMENT(details), "guide-more");
	if (open)
		htmx_element_set_attribute(HTMX_ELEMENT(details), "open", "open");
	htmx_node_set_text_content(HTMX_NODE(head), summary);
	ADD(details, head);
	ADD(details, axis_web_storage_text(NULL, text));
	values_fresh(&fresh, kind_name, kind);
	ADD(details, source_form(kind, request, "/indexers", submitted != NULL ? submitted : &fresh, TRUE, FALSE,
		"Add the indexer", prefix, back));
	values_clear(&fresh);
	return HTMX_NODE(details);
}

/*
 * A built-in site definition's block (add-builtin-torrent-indexers): what
 * the site is in its definition's sentence, and the form that adds it,
 * switched on and with no key.  Nothing is asked of the site until it is
 * added.
 */
static HtmxNode *
builtin_block(
	HtmxRequest                     *request,
	const AxisIndexerDefinitionInfo *info,
	const gchar                     *back
){
	HtmxDetails *details = htmx_details_new();
	HtmxSummary *head = htmx_summary_new();
	g_autofree gchar *summary = g_strdup_printf("%s, for movies and TV", info->name);
	g_autofree gchar *text = g_strdup_printf("%s It needs no account and no key, and nothing is asked of it until "
		"you add it here. Its address changes from time to time; change it here when the site stops answering.",
		info->description != NULL ? info->description : "");
	g_autofree gchar *prefix = g_strdup_printf("new-%s", info->def_id);
	HtmxNode *form = axis_web_indexers_definition_add_form(request, info, prefix);
	HtmxNode *back_field = axis_web_setup_return_field(back);

	htmx_element_add_class(HTMX_ELEMENT(details), "guide-more");
	htmx_node_set_text_content(HTMX_NODE(head), summary);
	ADD(details, head);
	ADD(details, axis_web_storage_text(NULL, text));
	if (back_field != NULL)
		ADD(form, back_field);
	ADD(details, form);
	return HTMX_NODE(details);
}

/* @result (nullable): a search of them all, with what was typed;
 * @definitions (nullable): the installed definitions. */
static HtmxResponse *
list_page(
	AxisWebIndexers               *self,
	HtmxRequest                   *request,
	GPtrArray                     *sources,
	GPtrArray                     *definitions,
	const gchar                   *notice,
	gboolean                       notice_error,
	const Values                  *submitted,
	const gchar                   *back,
	const Query                   *query,
	const AxisIndexerSearchResult *result
){
	g_autoptr(HtmxDiv) content = htmx_div_new();
	const gchar *submitted_kind = submitted != NULL ? submitted->kind : NULL;
	AxisWebNotice place;
	HtmxNode *card;
	gboolean plain = FALSE;
	guint i;

	list_intro(content);
	axis_web_notice_begin(&place, request, notice_error, g_strdup(notice), HTMX_NODE(content));
	card = axis_web_storage_card("Your indexers");
	if (sources->len == 0) {
		ADD(card, axis_web_storage_text("help", "No indexer has been added yet. Add one below, under Add an indexer."));
	} else {
		HtmxNode *table = axis_web_table_new("indexer-sources", source_columns);
		HtmxTbody *body = htmx_tbody_new();

		for (i = 0; i < sources->len; i++) {
			const AxisIndexerSourceInfo *source = g_ptr_array_index(sources, i);

			plain = plain || source->plain_http;
			ADD(body, source_row(self, request, source));
		}
		ADD(table, body);
		ADD(card, table);
		if (plain)
			ADD(card, axis_web_storage_text("help", "\"Not encrypted\": its address is plain http. That is usual "
				"inside one machine or a compose network; across a network, use https."));
		ADD(card, axis_web_storage_text("help", "\"Allowance today\" is what an indexer reports of its daily "
			"allowances (API calls and files) and what Axis has used since; automatic searches pass an indexer over "
			"when a tenth of its calls is left (at least five), keeping those for searches you ask for."));
	}
	ADD(content, card);

	if (sources->len > 0) {
		Query empty;

		card = axis_web_storage_card("Search every indexer");
		ADD(card, axis_web_storage_text(NULL, "Searches every indexer that is switched on, as a movie's or "
			"a series' search does, and shows what each found and which indexer each release came from. Nothing is "
			"downloaded, and what you type here is not kept or written to a log."));
		memset(&empty, 0, sizeof(empty));
		ADD(card, search_form(request, "/indexers/search", query != NULL ? query : &empty,
			"Search every indexer"));
		ADD(content, card);
		if (result != NULL)
			ADD(content, results_card(result, NULL, sources));
	}

	card = axis_web_storage_card("Add an indexer");
	if (axis_indexers_n_kinds(self->indexers) == 0) {
		ADD(card, axis_web_storage_text("help", "This version of Axis has no kind of indexer built in."));
	} else {
		HtmxNode *note = axis_web_setup_back_note(back);
		HtmxNode *block;
		gboolean none = sources->len == 0;

		if (note != NULL)
			ADD(card, note);
		block = add_block(self, request, AXIS_INDEXER_NEWZNAB_NAME, "A Usenet indexer (Newznab)",
			"Every Usenet indexer speaks Newznab. Give its API address, often https://api. and its name (such as "
			"https://api.example.com), and your API key: it is on the indexer's site, in your profile or on its API "
			"page.", g_strcmp0(submitted_kind, AXIS_INDEXER_NEWZNAB_NAME) == 0 ? submitted : NULL,
			g_strcmp0(submitted_kind, AXIS_INDEXER_NEWZNAB_NAME) == 0 || (none && submitted_kind == NULL), back);
		if (block != NULL)
			ADD(card, block);
		block = add_block(self, request, AXIS_INDEXER_TORZNAB_NAME, "A torrent indexer, through a Torznab feed",
			"Torznab is the same for torrents: a feed of Jackett's for one tracker, or one of Prowlarr's indexers on "
			"its own. Its address and the feed's path are shown where the feed is (Jackett's \"Copy Torznab Feed\"), "
			"with the key beside it.", g_strcmp0(submitted_kind, AXIS_INDEXER_TORZNAB_NAME) == 0 ? submitted : NULL,
			g_strcmp0(submitted_kind, AXIS_INDEXER_TORZNAB_NAME) == 0, back);
		if (block != NULL)
			ADD(card, block);
		block = add_block(self, request, AXIS_INDEXER_YTS_NAME, "YTS, for movies",
			"YTS lists movies, and Axis asks it by the movie's IMDb identifier, or by its title when there is none. "
			"It needs no account and no key. Its address changes from time to time: the one filled in is where it was "
			"when this version of Axis was made, and Axis never looks for a new one, so change it here when the site "
			"stops answering.", g_strcmp0(submitted_kind, AXIS_INDEXER_YTS_NAME) == 0 ? submitted : NULL,
			g_strcmp0(submitted_kind, AXIS_INDEXER_YTS_NAME) == 0, back);
		if (block != NULL)
			ADD(card, block);
		block = add_block(self, request, AXIS_INDEXER_EZTV_NAME, "EZTV, for TV",
			"EZTV lists TV episodes, and Axis asks it by the series' IMDb identifier only (it cannot be asked by "
			"words): a series without one finds nothing there. It needs no account and no key. Its address changes "
			"from time to time; change it here when the site stops answering.",
			g_strcmp0(submitted_kind, AXIS_INDEXER_EZTV_NAME) == 0 ? submitted : NULL,
			g_strcmp0(submitted_kind, AXIS_INDEXER_EZTV_NAME) == 0, back);
		if (block != NULL)
			ADD(card, block);
		block = add_block(self, request, AXIS_INDEXER_PIRATEBAY_NAME, "The Pirate Bay, for movies and TV",
			"The Pirate Bay is a public torrent index; Axis asks its own API (apibay) for a movie by its IMDb "
			"identifier, then by its title and year, and for a series' episode by its title with the season and "
			"episode, in the site's video categories. It needs no account and no key. Its address changes from time "
			"to time; change it here when the site stops answering.",
			g_strcmp0(submitted_kind, AXIS_INDEXER_PIRATEBAY_NAME) == 0 ? submitted : NULL,
			g_strcmp0(submitted_kind, AXIS_INDEXER_PIRATEBAY_NAME) == 0, back);
		if (block != NULL)
			ADD(card, block);
		/* The site definitions Axis ships, ready to add. */
		for (i = 0; axis_indexers_find_kind(self->indexers, AXIS_INDEXER_CARDIGANN_NAME) != NULL &&
		     definitions != NULL && i < definitions->len; i++) {
			const AxisIndexerDefinitionInfo *info = g_ptr_array_index(definitions, i);

			if (info->built_in && info->usable)
				ADD(card, builtin_block(request, info, back));
		}
		block = add_block(self, request, AXIS_INDEXER_PROWLARR_NAME, "Prowlarr, for every indexer in it",
			"Prowlarr keeps your indexers and searches them all with one request. An indexer also added here "
			"directly (the same address) is asked directly, not through Prowlarr.",
			g_strcmp0(submitted_kind, AXIS_INDEXER_PROWLARR_NAME) == 0 ? submitted : NULL,
			g_strcmp0(submitted_kind, AXIS_INDEXER_PROWLARR_NAME) == 0, back);
		if (block != NULL)
			ADD(card, block);
	}
	ADD(content, card);
	if (axis_indexers_find_kind(self->indexers, AXIS_INDEXER_CARDIGANN_NAME) != NULL) {
		ADD(content, axis_web_indexers_definitions_card(request, definitions, &place));
	}
	axis_web_notice_end(&place);
	return axis_web_page_respond(request, "Indexers", HTMX_NODE(content));
}

HtmxResponse *
axis_web_indexers_list_response(
	AxisWebIndexers *self,
	HtmxRequest     *request,
	GPtrArray       *sources,
	GPtrArray       *definitions,
	const gchar     *notice,
	gboolean         notice_error,
	guint            status
){
	HtmxResponse *response = list_page(self, request, sources, definitions, notice, notice_error, NULL, NULL, NULL,
		NULL);

	if (status != 0)
		htmx_response_set_status(response, status);
	return response;
}

typedef struct
{
	gchar                   *done;
	GPtrArray               *sources;
	GPtrArray               *definitions;
	Query                    query;
	AxisIndexerSearchResult *result;
	gchar                   *search_error;
	gboolean                 invalid_request;
	GError                  *error;
} ListWork;

static void
list_work_free(gpointer data)
{
	ListWork *work = data;

	g_free(work->done);
	g_clear_pointer(&work->sources, g_ptr_array_unref);
	g_clear_pointer(&work->definitions, g_ptr_array_unref);
	query_clear(&work->query);
	g_clear_pointer(&work->result, axis_indexer_search_result_free);
	g_free(work->search_error);
	g_clear_error(&work->error);
	g_free(work);
}

static void
list_run(
	AxisWebIndexers *self,
	gpointer         data
){
	ListWork *work = data;

	if (axis_indexers_list_sources(self->indexers, &work->sources, &work->error))
		axis_indexers_list_definitions(self->indexers, &work->definitions, &work->error);
}

static HtmxResponse *
list_done(
	AxisWebIndexers *self,
	HtmxRequest     *request,
	gpointer         data
){
	ListWork *work = data;
	gboolean is_error = FALSE;
	const gchar *message;

	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	message = done_message(work->done, &is_error);
	return list_page(self, request, work->sources, work->definitions, message, is_error, NULL, NULL, NULL, NULL);
}

static HtmxResponse *
handle_list(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	ListWork *work = g_new0(ListWork, 1);
	const gchar *done = htmx_request_get_query_param(request, "done");
	gboolean is_error = FALSE;

	if (done_message(done, &is_error) != NULL)
		work->done = g_strdup(done);
	return axis_web_indexers_dispatch(user_data, request, list_run, list_done, work, list_work_free);
}

/* ── A search of them all ───────────────────────────────────────── */

static void
search_all_run(
	AxisWebIndexers *self,
	gpointer         data
){
	ListWork *work = data;
	AxisIndexerRequest request;
	g_autoptr(GError) failure = NULL;
	const gchar *problem = NULL;
	g_autofree gchar *imdb = work->query.imdb != NULL ? g_strstrip(g_strdup(work->query.imdb)) : NULL;

	if (request_from_query(&work->query, &request, &problem)) {
		request.title = work->query.title;
		request.imdb_id = imdb != NULL && imdb[0] != '\0' ? imdb : NULL;
		/* A person asked: no indexer is passed over for its reserve. */
		request.automatic = FALSE;
		if (!axis_indexers_search(self->indexers, 0, &request, NULL, &work->result, &failure)) {
			work->search_error = g_strdup(failure->message);
			work->invalid_request = g_error_matches(failure, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_INVALID);
		}
	} else {
		work->search_error = g_strdup(problem);
		work->invalid_request = TRUE;
	}
	/* Their states as they are now. */
	if (axis_indexers_list_sources(self->indexers, &work->sources, &work->error))
		axis_indexers_list_definitions(self->indexers, &work->definitions, &work->error);
}

static HtmxResponse *
search_all_done(
	AxisWebIndexers *self,
	HtmxRequest     *request,
	gpointer         data
){
	ListWork *work = data;
	g_autofree gchar *notice = NULL;
	HtmxResponse *response;

	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	if (work->search_error != NULL) {
		notice = g_strdup_printf("The search did not give a result: %s", work->search_error);
		response = list_page(self, request, work->sources, work->definitions, notice, TRUE, NULL, NULL, &work->query,
			NULL);
		if (work->invalid_request)
			htmx_response_set_status(response, 400);
		return response;
	}
	return list_page(self, request, work->sources, work->definitions, NULL, FALSE, NULL, NULL, &work->query,
		work->result);
}

static HtmxResponse *
handle_search_all(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	ListWork *work = g_new0(ListWork, 1);

	query_from_form(&work->query, request);
	return axis_web_indexers_dispatch(user_data, request, search_all_run, search_all_done, work, list_work_free);
}

/* ── One indexer or source ──────────────────────────────────────── */

static const AxisWebColumn indexer_columns[] = {
	{ "Indexer", AXIS_WEB_COL_NAME },
	{ "Protocol", AXIS_WEB_COL_TEXT },
	{ "State", AXIS_WEB_COL_STATE },
	{ "Movies by", AXIS_WEB_COL_TEXT },
	{ "Episodes by", AXIS_WEB_COL_TEXT },
	{ "Downloads", AXIS_WEB_COL_TEXT },
	{ NULL, 0 }
};

/* The indexers of @source that are on and redirect their downloads to their
 * own sites; %NULL when none. */
static GPtrArray *
redirecting(const AxisIndexerSourceInfo *source)
{
	GPtrArray *names = NULL;
	guint i;

	for (i = 0; source->indexers != NULL && i < source->indexers->len; i++) {
		const AxisIndexerInfo *info = g_ptr_array_index(source->indexers, i);

		if (!info->enabled || !info->redirect)
			continue;
		if (names == NULL)
			names = g_ptr_array_new();
		g_ptr_array_add(names, info->name);
	}
	return names;
}

/* "NewsOwl", "NewsOwl and DozyNewt", "A, B and C". */
static gchar *
names_text(GPtrArray *names)
{
	GString *text = g_string_new(NULL);
	guint i;

	for (i = 0; i < names->len; i++)
		g_string_append_printf(text, "%s%s", i == 0 ? "" : i + 1 == names->len ? " and " : ", ",
			(const gchar *)g_ptr_array_index(names, i));
	return g_string_free(text, FALSE);
}

/* What the State card says of the indexers that redirect their downloads,
 * or %NULL when none does. */
static HtmxNode *
redirect_note(
	AxisWebIndexers             *self,
	const AxisIndexerSourceInfo *source
){
	g_autoptr(GPtrArray) names = source->native ? NULL : redirecting(source);
	g_autofree gchar *who = NULL;
	g_autofree gchar *text = NULL;
	const gchar *verb;

	if (names == NULL)
		return NULL;
	who = names_text(names);
	verb = names->len == 1 ? "redirects its" : "redirect their";
	if (!source->follow_redirects) {
		text = g_strdup_printf("%s %s downloads to the indexer's own site (%s's Redirect option), and following "
			"those redirects is switched off for this indexer, so %s releases cannot be fetched. Switch on \"Follow "
			"download redirects to the indexers\" under Settings for this indexer below, or turn off Redirect for %s in %s.", who,
			verb, kind_label(self, source->kind), names->len == 1 ? "its" : "their",
			names->len == 1 ? "that indexer" : "those indexers", kind_label(self, source->kind));
		return axis_web_storage_text("notice notice-warning", text);
	}
	text = g_strdup_printf("%s %s downloads to the indexer's own site. Axis fetches those releases from there: it "
		"follows that one redirect, over https and only to a public address, and sends nothing of its own.", who,
		verb);
	return axis_web_storage_text("help", text);
}

/* Where an indexer's downloads come from, for its row. */
static HtmxNode *
downloads_cell(
	AxisWebIndexers             *self,
	const AxisIndexerSourceInfo *source,
	const AxisIndexerInfo       *info
){
	HtmxTd *cell = htmx_td_new();
	g_autofree gchar *through = g_strdup_printf("through %s", kind_label(self, source->kind));
	HtmxSpan *text = htmx_span_new();

	htmx_node_set_text_content(HTMX_NODE(text), info->redirect ? "redirects to the indexer" : through);
	ADD(cell, text);
	if (info->redirect && info->enabled && !source->follow_redirects)
		ADD(cell, axis_web_browser_badge("Not fetched", "badge-warn"));
	return HTMX_NODE(cell);
}

static HtmxNode *
indexers_card(
	AxisWebIndexers             *self,
	const AxisIndexerSourceInfo *source
){
	HtmxNode *card = axis_web_storage_card("Indexers in this Prowlarr");
	guint on;
	guint failing;
	guint i;

	if (source->indexers->len == 0) {
		ADD(card, axis_web_storage_text("help", "None read yet. Test Prowlarr to read them."));
		return card;
	}
	count_indexers(source, &on, &failing);
	{
		GString *summary = g_string_new(NULL);
		g_autofree gchar *total = axis_web_count(source->indexers->len, "indexer", "indexers");
		g_autofree gchar *n_on = axis_web_format_count(on);
		g_autofree gchar *n_failing = axis_web_format_count(failing);

		g_string_append_printf(summary, "%s of %s %s on", n_on, total, source->indexers->len == 1 ? "is" : "are");
		if (failing > 0)
			g_string_append_printf(summary, ", and %s of those %s failing now (Prowlarr has disabled %s after "
				"failures)", n_failing, failing == 1 ? "is" : "are", failing == 1 ? "it" : "them");
		g_string_append_c(summary, '.');
		ADD(card, axis_web_storage_text(NULL, summary->str));
		g_string_free(summary, TRUE);
	}
	{
		HtmxNode *table = axis_web_table_new("indexer-list", indexer_columns);
		HtmxTbody *body = htmx_tbody_new();

		for (i = 0; i < source->indexers->len; i++) {
			const AxisIndexerInfo *info = g_ptr_array_index(source->indexers, i);
			HtmxTr *row = htmx_tr_new();
			HtmxTd *name = htmx_td_new();
			g_autofree gchar *films = axis_indexer_ids_label(info->movie_ids);
			g_autofree gchar *episodes = axis_indexer_ids_label(info->tv_ids);

			ADD(name, name_node(info->name));
			ADD(row, name);
			ADD(row, cell_text(axis_download_protocol_label(info->protocol)));
			if (!info->enabled)
				ADD(row, cell_node(axis_web_browser_badge("Off", "badge-note")));
			else if (info->failing)
				ADD(row, cell_node(axis_web_browser_badge("Failing", "badge-failed")));
			else
				ADD(row, cell_node(axis_web_browser_badge("On", "badge-succeeded")));
			ADD(row, cell_text(films));
			ADD(row, cell_text(episodes));
			ADD(row, downloads_cell(self, source, info));
			ADD(body, row);
		}
		ADD(table, body);
		ADD(card, table);
	}
	ADD(card, axis_web_storage_text("help", "\"Movies by\" and \"Episodes by\" are the identifiers each indexer says "
		"it takes; where it takes none, Axis searches it by the title as well. \"Downloads\" says whether a release "
		"comes through Prowlarr or Prowlarr redirects its download to the indexer's own site. An indexer also "
		"added here directly is asked directly and not through Prowlarr."));
	return card;
}

/* "IMDb (t=movie)" and so on, for a search mode's row. */
static gchar *
mode_text(
	gboolean     available,
	guint        ids,
	gboolean     text,
	gboolean     numbers,
	gboolean     tv
){
	GString *out;

	if (!available)
		return g_strdup("not offered");
	out = g_string_new(NULL);
	if (ids != 0) {
		g_autofree gchar *names = axis_indexer_ids_label(ids);

		g_string_append(out, names);
	}
	if (text)
		g_string_append_printf(out, "%swords", out->len > 0 ? ", " : "");
	if (tv)
		g_string_append(out, numbers ? "; with a season and an episode" : "; without a season and episode");
	if (out->len == 0)
		g_string_append(out, "offered, with nothing it says it takes");
	return g_string_free(out, FALSE);
}

/* A native indexer's capabilities. */
static HtmxNode *
caps_card(
	const AxisIndexerSourceInfo *source,
	gboolean                     site
){
	const AxisIndexerCaps *caps = &source->caps;
	HtmxNode *card = axis_web_storage_card("What this indexer can search by");
	HtmxNode *facts;
	g_autofree gchar *read = axis_web_format_time(caps->read_us, "never");
	g_autofree gchar *movies = NULL;
	g_autofree gchar *tv = NULL;
	g_autofree gchar *words = NULL;

	if (site && caps->read_us == 0) {
		ADD(card, axis_web_storage_text("help", "Not asked yet: a test or a first search shows what it covers."));
		return card;
	}
	if (caps->read_us == 0) {
		ADD(card, axis_web_storage_text("help", "Not read yet: until it is tested, it is searched by words only. A "
			"test reads its capabilities (t=caps), which say which identifiers it takes."));
		return card;
	}
	if (caps->problem != NULL) {
		g_autofree gchar *text = g_strdup_printf("Its capabilities could not be read when last asked (%s): it is "
			"searched by words only. Test it again to read them.", caps->problem);

		ADD(card, axis_web_storage_text("notice notice-warning", text));
		return card;
	}
	movies = mode_text((caps->modes & AXIS_INDEXER_CAN_MOVIE) != 0, caps->movie_ids, caps->movie_text, FALSE, FALSE);
	tv = mode_text((caps->modes & AXIS_INDEXER_CAN_TV) != 0, caps->tv_ids, caps->tv_text, caps->tv_numbers, TRUE);
	words = g_strdup((caps->modes & AXIS_INDEXER_CAN_SEARCH) != 0 ? "words" : "not offered");
	facts = axis_web_browser_facts_new();
	axis_web_browser_fact(facts, "Movies", movies);
	axis_web_browser_fact(facts, "TV", tv);
	axis_web_browser_fact(facts, "Any search by words", words);
	if (caps->limit_max > 0) {
		g_autofree gchar *max = axis_web_format_count((guint64)caps->limit_max);
		g_autofree gchar *usual = axis_web_format_count((guint64)MAX(caps->limit_default, 0));
		g_autofree gchar *limit = caps->limit_default > 0 ? g_strdup_printf("%s (%s unless asked)", max, usual) :
			g_strdup(max);

		axis_web_browser_fact(facts, "Results per search", limit);
	}
	if (caps->categories != NULL) {
		GString *shown = g_string_new(NULL);
		guint i;

		/* Its movie and TV categories: what can go in the settings. */
		for (i = 0; caps->categories[i] != NULL; i++) {
			if (caps->categories[i][0] != '2' && caps->categories[i][0] != '5')
				continue;
			if (strlen(caps->categories[i]) < 5 || caps->categories[i][4] != ' ')
				continue;
			g_string_append_printf(shown, "%s%s", shown->len > 0 ? "; " : "", caps->categories[i]);
		}
		if (shown->len > 0)
			axis_web_browser_fact(facts, "Its Newznab categories for movies and TV", shown->str);
		g_string_free(shown, TRUE);
	}
	axis_web_browser_fact(facts, site ? "Known since" : "Last read", read);
	ADD(card, facts);
	if (site) {
		ADD(card, axis_web_storage_text("help", "A site is not asked what it can do: Axis knows what each of these "
			"takes. A search that it cannot answer finds nothing there and says why."));
		return card;
	}
	ADD(card, axis_web_storage_text("help", "A search for a movie or an episode uses the first identifier it takes "
		"(IMDb, then TMDb for movies; TVDB, then IMDb, then TMDb for TV), and words where it takes none. They are "
		"read at a test and again every day."));
	return card;
}

/* The State card's facts of a native indexer. */
static void
native_facts(
	AxisWebIndexers             *self,
	const AxisIndexerSourceInfo *source,
	HtmxNode                    *facts
){
	const AxisIndexerAllowance *a = &source->allowance;
	const AxisIndexerKindInfo *info = axis_indexers_find_kind(self->indexers, source->kind);
	gint64 now_us = g_get_real_time();
	g_autofree gchar *used = allowance_text(a);
	gboolean site = info != NULL && info->site;

	axis_web_browser_fact(facts, "Protocol", source->protocol == AXIS_DOWNLOAD_USENET ? "Usenet" : "Torrent");
	if (site && info->definition) {
		/* One that runs a definition: where, and which. */
		g_autofree gchar *address = g_strconcat(source->address, source->base_path, NULL);

		axis_web_browser_fact(facts, "Address", address);
		axis_web_browser_fact(facts, "Site definition", source->definition_name != NULL ? source->definition_name :
			"(it is no longer installed)");
	} else if (site) {
		/* A site moves: the address it is asked at, and the one it was
		 * known at. */
		AxisHttpOrigin known_origin;
		gboolean same = FALSE;
		g_autofree gchar *known = NULL;

		/* An address is kept with its port: compared as it is kept. */
		if (axis_http_origin_parse(info->default_address, &known_origin, NULL)) {
			same = g_strcmp0(source->address, known_origin.base) == 0;
			axis_http_origin_clear(&known_origin);
		}
		known = same ? g_strdup_printf("%s (the address in use)", info->default_address) :
			g_strdup_printf("%s (changed here)", info->default_address);
		axis_web_browser_fact(facts, "Address", source->address);
		axis_web_browser_fact(facts, "Default address", known);
		axis_web_browser_fact(facts, "Covers", info->covers != NULL ? info->covers : "");
	} else if (used != NULL) {
		g_autofree gchar *reported = axis_web_format_time(a->reported_us, "");
		g_autofree gchar *text = g_strdup_printf("%s (as of %s)", used, reported);

		axis_web_browser_fact(facts, "Allowance today", text);
		if (a->api_max > 0) {
			g_autofree gchar *stop = axis_web_browser_format_count((guint64)MAX(a->api_max -
				axis_indexer_allowance_reserve(a->api_max), 0));
			g_autofree gchar *line = g_strdup_printf("at %s calls", stop);

			axis_web_browser_fact(facts, "Automatic searches stop", line);
		}
	} else {
		axis_web_browser_fact(facts, "Allowance today", "not reported by the indexer");
	}
	if (source->held_until_us > now_us || source->grabs_held_until_us > now_us) {
		g_autofree gchar *until = clock_text(MAX(source->held_until_us, source->grabs_held_until_us));
		g_autofree gchar *text = g_strdup_printf("until %s%s%s", until, source->hold_message != NULL ? ": " : "",
			source->hold_message != NULL ? source->hold_message : "");

		axis_web_browser_fact(facts, source->held_until_us > now_us ? "Not asked" : "No files fetched", text);
	}
	if (!site)
		axis_web_browser_fact(facts, "Files fetched from", source->files_origin != NULL ? source->files_origin :
			"from its own address");
	{
		g_autofree gchar *priority = source->priority == AXIS_INDEXER_DEFAULT_PRIORITY ?
			g_strdup_printf("%d (the default)", source->priority) : g_strdup_printf("%d", source->priority);

		axis_web_browser_fact(facts, "Priority", priority);
	}
}

/* The notice and button for an origin of files a test saw. */
static HtmxNode *
files_origin_note(
	HtmxRequest                 *request,
	const AxisIndexerSourceInfo *source
){
	g_autofree gchar *action = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *button = NULL;
	HtmxDiv *block;
	HtmxNode *form;

	if (!source->native || source->files_origin_seen == NULL)
		return NULL;
	/* The notice, with its one action inside it. */
	block = htmx_div_new();
	htmx_element_add_class(HTMX_ELEMENT(block), "notice");
	htmx_element_add_class(HTMX_ELEMENT(block), "notice-warning");
	action = g_strdup_printf("/indexers/%" G_GINT64_FORMAT "/accept-files", source->id);
	text = g_strdup_printf("This indexer serves its files from %s, not from its own address. Axis fetches files only "
		"from an indexer's own address and from one you accept for it, so its releases there are left out. Accept "
		"it if that is where this indexer's downloads come from.", source->files_origin_seen);
	button = g_strdup_printf("Accept files from %s", source->files_origin_seen);
	ADD(block, axis_web_storage_text(NULL, text));
	form = axis_web_auth_post_form(request, action);
	htmx_element_add_class(HTMX_ELEMENT(form), "inline-form");
	ADD(form, axis_web_auth_button(button, "button-small"));
	ADD(block, form);
	return HTMX_NODE(block);
}

typedef struct
{
	gint64                    id;
	gchar                    *done;
	AxisIndexerSourceInfo    *source;
	AxisIndexerSearchResult  *result;	/* a test search's */
	gchar                    *search_error;
	gboolean                  invalid_request;	/* the form's to fix */
	Query                     query;
	GError                   *error;
} SourceWork;

static void
source_work_free(gpointer data)
{
	SourceWork *work = data;

	g_free(work->done);
	g_clear_pointer(&work->source, axis_indexer_source_info_free);
	g_clear_pointer(&work->result, axis_indexer_search_result_free);
	g_free(work->search_error);
	query_clear(&work->query);
	g_clear_error(&work->error);
	g_free(work);
}

static void
source_run(
	AxisWebIndexers *self,
	gpointer         data
){
	SourceWork *work = data;

	axis_indexers_get_source(self->indexers, work->id, &work->source, &work->error);
}

static HtmxResponse *
source_page(
	AxisWebIndexers             *self,
	HtmxRequest                 *request,
	const AxisIndexerSourceInfo *source,
	const gchar                 *notice,
	gboolean                     notice_error,
	const Values                *submitted,
	const Query                 *query,
	const AxisIndexerSearchResult *result,
	guint                        status,
	const gchar                 *back
){
	g_autoptr(HtmxDiv) content = htmx_div_new();
	HtmxNode *card = axis_web_storage_card("State of this indexer");
	HtmxNode *facts = axis_web_browser_facts_new();
	g_autofree gchar *action = g_strdup_printf("/indexers/%" G_GINT64_FORMAT, source->id);
	g_autofree gchar *search = g_strdup_printf("/indexers/%" G_GINT64_FORMAT "/search", source->id);
	g_autofree gchar *delete = g_strdup_printf("/indexers/%" G_GINT64_FORMAT "/delete", source->id);
	g_autofree gchar *checked = axis_web_format_time(source->last_checked_us, "never");
	g_autofree gchar *contact = axis_web_format_time(source->last_contact_us, "never");
	g_autofree gchar *listed = axis_web_format_time(source->indexers_us, "never");
	g_autofree gchar *kept = axis_web_format_count(source->n_searches);
	g_autofree gchar *kind = kind_text(self, source);
	const AxisIndexerKindInfo *info = axis_indexers_find_kind(self->indexers, source->kind);
	gboolean site = info != NULL && info->site;
	HtmxResponse *response;
	AxisWebNotice place;
	HtmxNode *form;
	Values current;
	Query empty;

	ADD(content, heading(1, source->name));
	axis_web_notice_begin(&place, request, notice_error, g_strdup(notice), HTMX_NODE(content));
	htmx_element_set_id(HTMX_ELEMENT(card), "state");
	axis_web_notice_put(&place, card, "state");

	axis_web_browser_fact(facts, "Kind", kind);
	axis_web_browser_fact_node(facts, "State", state_badge(source->state));
	if (source->state_message != NULL)
		axis_web_browser_fact(facts, source->state == AXIS_INDEXER_SOURCE_OK ? "Note" : "What the indexer said",
			source->state_message);
	axis_web_browser_fact(facts, site ? "Answer read as" : source->native ? "Version it reports" : "Version",
		source->version != NULL ? source->version : "not known yet");
	axis_web_browser_fact(facts, "Last looked at", checked);
	axis_web_browser_fact(facts, "Last answered", contact);
	if (source->native) {
		native_facts(self, source, facts);
	} else {
		axis_web_browser_fact(facts, "Indexer list last read", listed);
		axis_web_browser_fact(facts, "Download redirects", source->follow_redirects ? "followed to the indexers" :
			"not followed");
	}
	axis_web_browser_fact(facts, "Searches kept now", kept);
	ADD(card, facts);
	{
		HtmxNode *note = redirect_note(self, source);

		if (note != NULL)
			ADD(card, note);
		note = files_origin_note(request, source);
		if (note != NULL)
			ADD(card, note);
	}
	ADD(card, axis_web_indexers_test_form(request, source->id, "Test this indexer now", NULL, NULL));
	/* One that runs a definition: what it reads, step by step. */
	if (source->definition_id != 0) {
		g_autofree gchar *check = g_strdup_printf("/indexers/%" G_GINT64_FORMAT "/check", source->id);
		HtmxA *check_link = htmx_a_new_with_href(check);

		htmx_element_add_class(HTMX_ELEMENT(check_link), "button");
		htmx_node_set_text_content(HTMX_NODE(check_link), "Check this indexer");
		ADD(card, check_link);
		ADD(card, axis_web_storage_text("help", "Check this indexer: one search, shown step by step (what was asked, "
			"what came back, what each field read), to see why it finds nothing or fails."));
	}
	ADD(content, card);

	ADD(content, source->native ? caps_card(source, site) : indexers_card(self, source));

	card = axis_web_storage_card("Test search");
	ADD(card, axis_web_storage_text(NULL, source->native ? "Searches this indexer alone and shows the releases it "
		"found, as it named them. Nothing is downloaded, and what you type here is not kept or written to a log. Give "
		"the title of a movie or series, or the words to search for; the identifiers are optional, used where the "
		"indexer takes them, and the title is used when it does not." :
		"Searches this indexer and shows the releases it found, as Prowlarr named them. Nothing is downloaded, and "
		"what you type here is not kept or written to a log. Give the title of a movie or series, or the words to "
		"search for; the identifiers are optional, used where the indexers take them, and the title is used when "
		"they do not."));
	memset(&empty, 0, sizeof(empty));
	ADD(card, search_form(request, search, query != NULL ? query : &empty, "Search this indexer"));
	ADD(content, card);
	if (result != NULL)
		ADD(content, results_card(result, source, NULL));

	card = axis_web_storage_card("Settings for this indexer");
	htmx_element_set_id(HTMX_ELEMENT(card), "settings");
	axis_web_notice_put(&place, card, "settings");
	if (source->plain_http)
		ADD(card, plain_http_warning(!site));
	if (back != NULL)
		ADD(card, axis_web_setup_back_note(back));
	if (submitted == NULL)
		values_from_source(&current, source);
	ADD(card, source_form(axis_indexers_find_kind(self->indexers, source->kind), request, action,
		submitted != NULL ? submitted : &current, FALSE, source->key_set, "Save", "source", back));
	if (submitted == NULL)
		values_clear(&current);
	ADD(content, card);

	card = axis_web_storage_card("Remove this indexer");
	ADD(card, axis_web_storage_text(NULL, site ? "Axis forgets the site and the searches kept for it. Nothing at the "
		"site is changed." : source->native ? "Axis forgets the indexer, its key and the searches kept "
		"for it. Nothing at the indexer is changed." : "Axis forgets Prowlarr, its key, the indexers it reported "
		"and the searches kept for it. Nothing in Prowlarr is changed."));
	form = axis_web_auth_post_form(request, delete);
	htmx_element_add_class(HTMX_ELEMENT(form), "inline-form");
	ADD(form, axis_web_storage_checkbox("source-delete-confirm", "Remove it for good", "confirm", TRUE));
	ADD(form, axis_web_auth_button("Remove this indexer", "button-danger"));
	ADD(card, form);
	ADD(content, card);
	ADD(content, axis_web_browser_link("/indexers", "Back to Indexers"));
	axis_web_notice_end(&place);
	response = axis_web_page_respond(request, source->name, HTMX_NODE(content));
	if (status != 200)
		htmx_response_set_status(response, status);
	return response;
}

static HtmxResponse *
source_done(
	AxisWebIndexers *self,
	HtmxRequest     *request,
	gpointer         data
){
	SourceWork *work = data;
	gboolean is_error = FALSE;
	const gchar *message;

	if (g_error_matches(work->error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_NOT_FOUND))
		return axis_web_error_respond(request, 404, "Not found", "There is no such indexer.");
	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	message = done_message(work->done, &is_error);
	return source_page(self, request, work->source, message, is_error, NULL, NULL, NULL, 200, NULL);
}

static HtmxResponse *
handle_source(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	gint64 id = axis_web_storage_param_id(params);
	const gchar *done = htmx_request_get_query_param(request, "done");
	gboolean is_error = FALSE;
	SourceWork *work;

	if (id <= 0)
		return axis_web_error_respond(request, 404, "Not found", "There is no such indexer.");
	work = g_new0(SourceWork, 1);
	work->id = id;
	if (done_message(done, &is_error) != NULL)
		work->done = g_strdup(done);
	return axis_web_indexers_dispatch(user_data, request, source_run, source_done, work, source_work_free);
}

/* ── The test search ────────────────────────────────────────────── */

static void
search_run(
	AxisWebIndexers *self,
	gpointer         data
){
	SourceWork *work = data;
	AxisIndexerRequest request;
	g_autoptr(GError) failure = NULL;
	const gchar *problem = NULL;
	g_autofree gchar *imdb = work->query.imdb != NULL ? g_strstrip(g_strdup(work->query.imdb)) : NULL;

	if (!axis_indexers_get_source(self->indexers, work->id, &work->source, &work->error))
		return;
	if (!request_from_query(&work->query, &request, &problem)) {
		work->search_error = g_strdup(problem);
		work->invalid_request = TRUE;
		return;
	}
	request.title = work->query.title;
	request.imdb_id = imdb != NULL && imdb[0] != '\0' ? imdb : NULL;
	if (!axis_indexers_search(self->indexers, work->id, &request, NULL, &work->result, &failure)) {
		/* The layer's words about a request that cannot be made, and a
		 * source that did not answer, hold none of what was typed. */
		work->search_error = g_strdup(failure->message);
		work->invalid_request = g_error_matches(failure, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_INVALID);
	}
	/* Its state was recorded (and what it reported); show it as it is now. */
	g_clear_pointer(&work->source, axis_indexer_source_info_free);
	axis_indexers_get_source(self->indexers, work->id, &work->source, &work->error);
}

static HtmxResponse *
search_done(
	AxisWebIndexers *self,
	HtmxRequest     *request,
	gpointer         data
){
	SourceWork *work = data;
	g_autofree gchar *notice = NULL;

	if (g_error_matches(work->error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_NOT_FOUND))
		return axis_web_error_respond(request, 404, "Not found", "There is no such indexer.");
	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	if (work->search_error != NULL) {
		/* A request that could not be made is the form's to fix; a source
		 * that did not answer is shown as it is. */
		notice = g_strdup_printf("The search did not give a result: %s", work->search_error);
		return source_page(self, request, work->source, notice, TRUE, NULL, &work->query, NULL,
			work->invalid_request ? 400 : 200, NULL);
	}
	return source_page(self, request, work->source, NULL, FALSE, NULL, &work->query, work->result, 200, NULL);
}

static HtmxResponse *
handle_search(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	SourceWork *work;
	gint64 id = axis_web_storage_param_id(params);

	if (id <= 0)
		return axis_web_error_respond(request, 404, "Not found", "There is no such indexer.");
	work = g_new0(SourceWork, 1);
	work->id = id;
	query_from_form(&work->query, request);
	return axis_web_indexers_dispatch(user_data, request, search_run, search_done, work, source_work_free);
}

/* ── Changes ────────────────────────────────────────────────────── */

typedef enum
{
	CHANGE_SAVE,
	CHANGE_TEST,
	CHANGE_ACCEPT,
	CHANGE_DELETE
} ChangeId;

typedef struct
{
	ChangeId                change;
	gint64                  id;	/* the source; 0 for a new one */
	Values                  values;
	gchar                  *secret;	/* wiped */
	gint64                  saved_id;
	GError                 *error;
	AxisIndexerSourceInfo  *source;	/* to show the page again */
	GPtrArray              *sources;	/* the list, to show it again */
	GPtrArray              *definitions;	/* and the installed definitions */
	const gchar            *back;	/* the setup guide's address (static), or NULL */
} ChangeWork;

static void
change_work_free(gpointer data)
{
	ChangeWork *work = data;

	values_clear(&work->values);
	axis_secret_free(work->secret);
	g_clear_error(&work->error);
	g_clear_pointer(&work->source, axis_indexer_source_info_free);
	g_clear_pointer(&work->sources, g_ptr_array_unref);
	g_clear_pointer(&work->definitions, g_ptr_array_unref);
	g_free(work);
}

/* The priority as typed: empty is the default (0), anything else must be a
 * number (the layer checks its range); -1 when it is not one. */
static gint
priority_of(const gchar *text)
{
	gint64 value = 0;

	if (!number_of(text, 0, &value))
		return -1;
	return (gint)MIN(value, (gint64)G_MAXINT);
}

static void
change_run(
	AxisWebIndexers *self,
	gpointer         data
){
	ChangeWork *work = data;
	g_autoptr(GError) reread = NULL;

	switch (work->change) {
	case CHANGE_SAVE: {
		AxisIndexerSourceSpec spec;

		memset(&spec, 0, sizeof(spec));
		spec.name = work->values.name;
		spec.kind = work->values.kind;
		spec.address = work->values.address;
		spec.base_path = work->values.base_path;
		spec.accept_untrusted = work->values.accept_untrusted;
		spec.enabled = work->values.enabled;
		spec.refuse_redirects = !work->values.follow_redirects;
		spec.secret = work->secret;
		spec.priority = priority_of(work->values.priority);
		spec.movie_categories = work->values.movie_categories;
		spec.tv_categories = work->values.tv_categories;
		spec.files_origin = work->values.files_origin;
		if (work->values.definition != NULL)
			spec.definition_id = g_ascii_strtoll(work->values.definition, NULL, 10);
		axis_indexers_save_source(self->indexers, work->id, &spec, &work->saved_id, &work->error);
		break;
	}
	case CHANGE_TEST:
		axis_indexers_test_source(self->indexers, work->id, NULL, &work->error);
		break;
	case CHANGE_ACCEPT:
		axis_indexers_accept_files_origin(self->indexers, work->id, &work->error);
		break;
	default:
		axis_indexers_delete_source(self->indexers, work->id, &work->error);
		break;
	}
	/* The key is not needed beyond this. */
	g_clear_pointer(&work->secret, axis_secret_free);
	/* A refusal shows the page again: read what it shows. */
	if (work->error != NULL && work->error->domain == AXIS_INDEXER_ERROR &&
	    !g_error_matches(work->error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_NOT_FOUND)) {
		if (work->id == 0) {
			if (axis_indexers_list_sources(self->indexers, &work->sources, &reread))
				axis_indexers_list_definitions(self->indexers, &work->definitions, NULL);
		} else
			axis_indexers_get_source(self->indexers, work->id, &work->source, &reread);
	}
}

static HtmxResponse *
change_done(
	AxisWebIndexers *self,
	HtmxRequest     *request,
	gpointer         data
){
	ChangeWork *work = data;
	g_autofree gchar *location = NULL;

	if (g_error_matches(work->error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_NOT_FOUND))
		return axis_web_error_respond(request, 404, "Not found", "There is no such indexer.");
	if (work->error != NULL && work->error->domain == AXIS_INDEXER_ERROR && (work->source != NULL ||
	    work->sources != NULL)) {
		/* The layer's words name the field, never its value. */
		if (work->id == 0) {
			HtmxResponse *response = list_page(self, request, work->sources, work->definitions, work->error->message,
				TRUE, &work->values, work->back, NULL, NULL);

			htmx_response_set_status(response, 400);
			return response;
		}
		return source_page(self, request, work->source, work->error->message, TRUE,
			work->change == CHANGE_SAVE ? &work->values : NULL, NULL, NULL, 400, work->back);
	}
	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	switch (work->change) {
	case CHANGE_SAVE:
		axis_web_storage_audit(request, "indexer source %" G_GINT64_FORMAT " %s", work->saved_id,
			work->id == 0 ? "added" : "changed");
		if (work->back != NULL)
			location = axis_web_setup_return_location(work->back, work->id == 0 ? "added" : "saved");
		else {
			/* A changed one comes back to its Settings card. */
			g_autofree gchar *path = g_strdup_printf("/indexers/%" G_GINT64_FORMAT, work->saved_id);

			return axis_web_place_redirect(request, path, work->id == 0 ? "added" : "saved",
				work->id == 0 ? NULL : "settings");
		}
		break;
	case CHANGE_TEST:
		axis_web_storage_audit(request, "indexer source %" G_GINT64_FORMAT " tested", work->id);
		if (work->back != NULL)
			location = axis_web_setup_return_location(work->back, "tested");
		else {
			g_autofree gchar *path = g_strdup_printf("/indexers/%" G_GINT64_FORMAT, work->id);

			return axis_web_place_redirect(request, path, "tested", "state");
		}
		break;
	case CHANGE_ACCEPT: {
		g_autofree gchar *path = g_strdup_printf("/indexers/%" G_GINT64_FORMAT, work->id);

		axis_web_storage_audit(request, "indexer source %" G_GINT64_FORMAT " files address accepted", work->id);
		return axis_web_place_redirect(request, path, "accepted", "state");
	}
	default:
		axis_web_storage_audit(request, "indexer source %" G_GINT64_FORMAT " removed", work->id);
		location = g_strdup("/indexers?done=deleted");
		break;
	}
	return axis_web_auth_redirect(request, location);
}

static HtmxResponse *
start_change(
	AxisWebIndexers *self,
	HtmxRequest     *request,
	ChangeWork      *work
){
	return axis_web_indexers_dispatch(self, request, change_run, change_done, work, change_work_free);
}

static HtmxResponse *
handle_save(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	ChangeWork *work = g_new0(ChangeWork, 1);
	const gchar *secret = htmx_request_get_form_value(request, "secret");

	work->change = CHANGE_SAVE;
	work->id = params != NULL && g_hash_table_lookup(params, "id") != NULL ? axis_web_storage_param_id(params) : 0;
	if (params != NULL && g_hash_table_lookup(params, "id") != NULL && work->id <= 0) {
		change_work_free(work);
		return axis_web_error_respond(request, 404, "Not found", "There is no such indexer.");
	}
	values_from_form(&work->values, request);
	if (secret != NULL && secret[0] != '\0' && strlen(secret) <= 4096)
		work->secret = g_strdup(secret);
	work->back = axis_web_setup_request_return(request);
	return start_change(user_data, request, work);
}

static HtmxResponse *
handle_change(
	HtmxRequest *request,
	GHashTable  *params,
	AxisWebIndexers *self,
	ChangeId     change
){
	ChangeWork *work;
	gint64 id = axis_web_storage_param_id(params);

	if (id <= 0)
		return axis_web_error_respond(request, 404, "Not found", "There is no such indexer.");
	work = g_new0(ChangeWork, 1);
	work->change = change;
	work->id = id;
	if (change == CHANGE_TEST)
		work->back = axis_web_setup_request_return(request);
	return start_change(self, request, work);
}

static HtmxResponse *
handle_test(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	return handle_change(request, params, user_data, CHANGE_TEST);
}

static HtmxResponse *
handle_accept(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	return handle_change(request, params, user_data, CHANGE_ACCEPT);
}

static HtmxResponse *
handle_delete(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	if (axis_web_storage_param_id(params) > 0 && !axis_web_storage_form_yes(request, "confirm"))
		return axis_web_error_respond(request, 400, "Not confirmed", "Tick the box to remove the indexer.");
	return handle_change(request, params, user_data, CHANGE_DELETE);
}

void
axis_web_indexers_register_pages(
	AxisWebIndexers *self,
	AxisWeb         *web
){
	axis_web_route_full(web, HTMX_METHOD_GET, "/indexers", AXIS_WEB_ROUTE_ADMIN, handle_list, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/indexers", AXIS_WEB_ROUTE_ADMIN, handle_save, self);
	axis_web_indexers_register_definitions(self, web);
	axis_web_route_full(web, HTMX_METHOD_POST, "/indexers/search", AXIS_WEB_ROUTE_ADMIN, handle_search_all, self);
	axis_web_indexers_register_check(self, web);
	axis_web_route_full(web, HTMX_METHOD_GET, "/indexers/:id", AXIS_WEB_ROUTE_ADMIN, handle_source, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/indexers/:id", AXIS_WEB_ROUTE_ADMIN, handle_save, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/indexers/:id/test", AXIS_WEB_ROUTE_ADMIN, handle_test, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/indexers/:id/search", AXIS_WEB_ROUTE_ADMIN, handle_search, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/indexers/:id/accept-files", AXIS_WEB_ROUTE_ADMIN, handle_accept,
		self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/indexers/:id/delete", AXIS_WEB_ROUTE_ADMIN, handle_delete, self);
}
