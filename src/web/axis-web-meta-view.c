/*
 * axis-web-meta-view.c - pieces shared by the pages that show titles: the
 * result card, the poster's place, the identifiers' links
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Everything that came from a provider is set as text content or as an
 * attribute value, which htmx-glib escapes; nothing here assembles markup
 * by hand.  A link to another service is built only from an identifier that
 * has passed axis_meta_id_valid(), and an image address only from a content
 * hash that has passed axis_meta_artwork_hash_valid(): both are written by
 * Axis, never taken from an answer.
 */

#include "web/axis-web-meta-private.h"

#include <string.h>

#include "web/axis-web-browser-private.h"
#include "web/axis-web-storage-private.h"

#define ADD axis_web_meta_add

/* A placeholder's own name for the title, cut so that the address stays short. */
#define PLACEHOLDER_TITLE_BYTES 120

void
axis_web_meta_add(
	gpointer parent,
	gpointer child
){
	htmx_node_add_child(HTMX_NODE(parent), HTMX_NODE(child));
	g_object_unref(child);
}

gchar *
axis_web_meta_param(
	HtmxRequest *request,
	const gchar *name,
	gsize        max_bytes
){
	const gchar *value = htmx_request_get_query_param(request, name);

	if (value == NULL || value[0] == '\0' || strlen(value) > max_bytes || !g_utf8_validate(value, -1, NULL))
		return NULL;
	return g_strdup(value);
}

HtmxNode *
axis_web_meta_attribution(const AxisMetaProviderInfo *info)
{
	g_autofree gchar *text = g_strdup_printf("%s: %s", info->label, info->attribution);
	g_autofree gchar *spaced = NULL;
	HtmxP *line;
	HtmxSpan *words;

	if (info->attribution_url == NULL)
		return axis_web_storage_text("help attribution", text);
	/* A provider whose terms ask for a direct link to it (TheTVDB). */
	spaced = g_strconcat(text, " ", NULL);
	line = htmx_p_new();
	htmx_element_add_class(HTMX_ELEMENT(line), "help");
	htmx_element_add_class(HTMX_ELEMENT(line), "attribution");
	words = htmx_span_new();
	htmx_node_set_text_content(HTMX_NODE(words), spaced);
	ADD(line, words);
	ADD(line, axis_web_meta_attribution_link(info));
	return HTMX_NODE(line);
}

HtmxNode *
axis_web_meta_attribution_link(const AxisMetaProviderInfo *info)
{
	HtmxA *link = htmx_a_new_with_href(info->attribution_url);

	htmx_element_add_class(HTMX_ELEMENT(link), "attribution-link");
	htmx_node_set_text_content(HTMX_NODE(link), info->label);
	htmx_a_set_target(link, "_blank");
	htmx_a_set_rel(link, "noreferrer noopener");
	return HTMX_NODE(link);
}

gchar *
axis_web_meta_why_text(
	const gchar *label,
	const gchar *problem
){
	if (problem != NULL && problem[0] != '\0')
		return g_strdup_printf("%s%s", problem, g_str_has_suffix(problem, ".") ? "" : ".");
	return g_strdup_printf("%s did not answer.", label);
}

gchar *
axis_web_meta_age_text(gint64 us)
{
	gint64 seconds = (g_get_real_time() - us) / G_USEC_PER_SEC;

	if (seconds < 90)
		return g_strdup("just now");
	if (seconds < 90 * 60) {
		gint64 minutes = (seconds + 30) / 60;

		return g_strdup_printf("%" G_GINT64_FORMAT " minutes ago", minutes);
	}
	if (seconds < 48 * 3600) {
		gint64 hours = (seconds + 1800) / 3600;

		return g_strdup_printf("%" G_GINT64_FORMAT " hours ago", hours);
	}
	return g_strdup_printf("%" G_GINT64_FORMAT " days ago", (seconds + 43200) / 86400);
}

gchar *
axis_web_meta_when_text(gint64 us)
{
	g_autofree gchar *when = axis_web_format_time(us, "never");
	g_autofree gchar *age = NULL;

	if (us <= 0)
		return g_steal_pointer(&when);
	age = axis_web_meta_age_text(us);
	return g_strdup_printf("%s, %s", when, age);
}

/* The first @max bytes of @text, ending on a character. */
static gchar *
cut_text(
	const gchar *text,
	gsize        max
){
	gsize length = strlen(text);
	const gchar *end;

	if (length <= max)
		return g_strdup(text);
	end = text + max;
	while (end > text && (*end & 0xC0) == 0x80)
		end--;
	return g_strndup(text, (gsize)(end - text));
}

static gboolean
poster_loadable(const AxisMetaTitle *title)
{
	return title->provider != NULL && axis_meta_provider_name_valid(title->provider) &&
		title->poster_ref != NULL && axis_meta_image_ref_valid(title->poster_ref);
}

HtmxNode *
axis_web_meta_poster_new(
	const AxisMetaTitle *title,
	const gchar         *hash,
	gboolean             large,
	guint                attempt
){
	HtmxDiv *box = htmx_div_new();

	htmx_element_add_class(HTMX_ELEMENT(box), "poster");
	if (large)
		htmx_element_add_class(HTMX_ELEMENT(box), "poster-large");
	if (hash != NULL && axis_meta_artwork_hash_valid(hash)) {
		g_autofree gchar *src = g_strdup_printf("/art/%s", hash);
		/* The title is beside it: the picture adds nothing a reader of the
		 * page would miss. */
		HtmxImg *img = htmx_img_new_with_src(src, "");

		htmx_img_set_loading(img, large ? "eager" : "lazy");
		ADD(box, img);
		return HTMX_NODE(box);
	}

	htmx_element_add_class(HTMX_ELEMENT(box), "poster-none");
	{
		HtmxSpan *name = htmx_span_new();

		htmx_element_add_class(HTMX_ELEMENT(name), "poster-title");
		htmx_node_set_text_content(HTMX_NODE(name), title->title != NULL ? title->title : "");
		ADD(box, name);
	}
	if (poster_loadable(title) && attempt <= AXIS_WEB_META_POSTER_RETRIES) {
		g_autofree gchar *ref = g_uri_escape_string(title->poster_ref, NULL, FALSE);
		g_autofree gchar *cut = cut_text(title->title != NULL ? title->title : "", PLACEHOLDER_TITLE_BYTES);
		g_autofree gchar *name = g_uri_escape_string(cut, NULL, FALSE);
		g_autofree gchar *url = g_strdup_printf(AXIS_WEB_META_POSTER_PATH "?provider=%s&ref=%s&size=%s&try=%u&t=%s",
			title->provider, ref, large ? "page" : "card", attempt, name);

		htmx_element_set_attribute(HTMX_ELEMENT(box), "hx-get", url);
		/* The first load waits until the card is on screen; a retry (the
		 * image was still on its way) waits a moment. */
		htmx_element_set_attribute(HTMX_ELEMENT(box), "hx-trigger", attempt == 0 ? "revealed" : "load delay:2s");
		htmx_element_set_attribute(HTMX_ELEMENT(box), "hx-swap", "outerHTML");
	}
	return HTMX_NODE(box);
}

static HtmxNode *
card_line(const AxisMetaTitle *title)
{
	g_autofree gchar *text = title->year > 0 ?
		g_strdup_printf("%s, %d", title->kind == AXIS_META_KIND_SERIES ? "TV series" : "Movie", title->year) :
		g_strdup(title->kind == AXIS_META_KIND_SERIES ? "TV series" : "Movie");

	return axis_web_storage_text("title-card-line", text);
}

HtmxNode *
axis_web_meta_card_new(
	const AxisMetaTitle *title,
	const gchar         *poster_hash,
	HtmxNode            *corner,
	gboolean             admin
){
	return axis_web_meta_card_new_full(title, poster_hash, corner, admin, NULL, NULL, NULL);
}

static const gchar *
service_label(const gchar *scheme);

HtmxNode *
axis_web_meta_card_new_full(
	const AxisMetaTitle *title,
	const gchar         *poster_hash,
	HtmxNode            *corner,
	gboolean             admin,
	const gchar         *href,
	const gchar         *line,
	HtmxNode            *extra
){
	HtmxArticle *card = htmx_article_new();
	HtmxHeading *name = htmx_h3_new();

	htmx_element_add_class(HTMX_ELEMENT(card), "title-card");
	ADD(card, axis_web_meta_poster_new(title, poster_hash, FALSE, 0));

	/* The kind and year first, in one line: the title that follows has two
	 * lines of room, so what comes under it starts in one place. */
	ADD(card, line != NULL ? axis_web_storage_text("title-card-line", line) : card_line(title));
	htmx_element_add_class(HTMX_ELEMENT(name), "title-card-name");
	if (href != NULL || title->id > 0) {
		g_autofree gchar *own = href == NULL ? g_strdup_printf("/title/%" G_GINT64_FORMAT, title->id) : NULL;
		HtmxA *link = htmx_a_new_with_href(href != NULL ? href : own);

		htmx_node_set_text_content(HTMX_NODE(link), title->title);
		ADD(name, link);
	} else {
		htmx_node_set_text_content(HTMX_NODE(name), title->title);
	}
	ADD(card, name);
	/* Always there, and two lines high: every card is the same height whether
	 * it has an overview or not. */
	ADD(card, axis_web_storage_text("title-card-overview", title->overview != NULL ? title->overview : ""));
	if (extra != NULL)
		ADD(card, extra);
	if (admin) {
		g_autoptr(GString) ids = g_string_new(NULL);
		g_autofree gchar *fetched = axis_web_format_time(title->fetched_us, "never");
		g_autofree gchar *saved = g_strdup_printf("Saved %s", fetched);
		guint i;

		for (i = 0; title->ids != NULL && i < title->ids->len; i++) {
			const AxisMetaExternalId *id = g_ptr_array_index(title->ids, i);

			g_string_append_printf(ids, "%s%s %s", i > 0 ? ", " : "", service_label(id->scheme), id->value);
		}
		if (ids->len > 0) {
			HtmxP *p = htmx_p_new();

			htmx_element_add_class(HTMX_ELEMENT(p), "meta-ids");
			ADD(p, htmx_code_new_with_text(ids->str));
			ADD(card, p);
		}
		ADD(card, axis_web_storage_text("as-of", saved));
	}
	if (corner != NULL)
		ADD(card, corner);
	return HTMX_NODE(card);
}

gchar *
axis_web_meta_added_text(
	const gchar         *done,
	const AxisMetaTitle *title
){
	g_autofree gchar *label = NULL;

	if (title == NULL || (g_strcmp0(done, "added") != 0 && g_strcmp0(done, "managed") != 0))
		return NULL;
	label = title->year > 0 ? g_strdup_printf("%s (%d)", title->title, title->year) : g_strdup(title->title);
	if (g_strcmp0(done, "managed") == 0)
		return g_strdup_printf("Axis manages %s now: its files on disk count as in the library.", label);
	return g_strdup_printf("Added %s to %s. Its card shows where it stands now; its page has the details.", label,
		title->kind == AXIS_META_KIND_FILM ? "Movies" : "TV");
}

/* ── Identifiers ────────────────────────────────────────────────── */

/* The address of @title's page at the service that issued @id, or %NULL when
 * Axis does not link to it.  @id has been validated. */
static gchar *
service_url(
	const AxisMetaTitle      *title,
	const AxisMetaExternalId *id
){
	gboolean series = title->kind == AXIS_META_KIND_SERIES;

	if (strcmp(id->scheme, "imdb") == 0)
		return g_strdup_printf("https://www.imdb.com/title/%s/", id->value);
	if (strcmp(id->scheme, "tmdb") == 0)
		return g_strdup_printf("https://www.themoviedb.org/%s/%s", series ? "tv" : "movie", id->value);
	if (strcmp(id->scheme, "tvdb") == 0)
		return g_strdup_printf("https://thetvdb.com/dereferrer/%s/%s", series ? "series" : "movie", id->value);
	return NULL;
}

static const gchar *
service_label(const gchar *scheme)
{
	if (strcmp(scheme, "imdb") == 0)
		return "IMDb";
	if (strcmp(scheme, "tmdb") == 0)
		return "TMDb";
	if (strcmp(scheme, "tvdb") == 0)
		return "TVDB";
	return scheme;
}

HtmxNode *
axis_web_meta_id_links(const AxisMetaTitle *title)
{
	HtmxUl *list;
	guint i;

	if (title->ids == NULL || title->ids->len == 0)
		return NULL;
	list = htmx_ul_new();
	htmx_element_add_class(HTMX_ELEMENT(list), "title-links");
	for (i = 0; i < title->ids->len; i++) {
		const AxisMetaExternalId *id = g_ptr_array_index(title->ids, i);
		HtmxLi *item = htmx_li_new();
		g_autofree gchar *url = NULL;

		if (!axis_meta_id_valid(id->scheme, id->value))
			continue;
		url = service_url(title, id);
		if (url != NULL) {
			HtmxA *link = htmx_a_new_with_href(url);

			htmx_node_set_text_content(HTMX_NODE(link), service_label(id->scheme));
			htmx_a_set_target(link, "_blank");
			htmx_a_set_rel(link, "noreferrer noopener");
			ADD(item, link);
		} else {
			HtmxSpan *label = htmx_span_new();

			htmx_node_set_text_content(HTMX_NODE(label), service_label(id->scheme));
			ADD(item, label);
		}
		ADD(item, htmx_code_new_with_text(id->value));
		ADD(list, item);
	}
	return HTMX_NODE(list);
}
