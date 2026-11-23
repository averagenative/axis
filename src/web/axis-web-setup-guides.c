/*
 * axis-web-setup-guides.c - the setup guide's short guides: the words about
 * what lies outside Axis and goes wrong there (which TMDb value to paste, a
 * category of Axis's own, one disk for downloads and library, path
 * mappings, what writing means), and the pieces every step uses
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The longer ones are closed <details> blocks, so that a step stays short.
 * Every word here is fixed text of Axis's.
 */

#include "web/axis-web-setup-private.h"

#include <string.h>

#define ADD axis_web_setup_add

/* ── Pieces ─────────────────────────────────────────────────────── */

void
axis_web_setup_add(
	gpointer parent,
	gpointer child
){
	htmx_node_add_child(HTMX_NODE(parent), HTMX_NODE(child));
	g_object_unref(child);
}

HtmxNode *
axis_web_setup_text(
	const gchar *css,
	const gchar *text
){
	HtmxP *p = htmx_p_new();

	if (css != NULL)
		htmx_element_add_class(HTMX_ELEMENT(p), css);
	htmx_node_set_text_content(HTMX_NODE(p), text);
	return HTMX_NODE(p);
}

HtmxNode *
axis_web_setup_link_out(
	const gchar *href,
	const gchar *text
){
	HtmxA *link = htmx_a_new_with_href(href);
	HtmxSpan *words = htmx_span_new();
	HtmxSpan *note = htmx_span_new();

	htmx_node_set_text_content(HTMX_NODE(words), text);
	htmx_element_add_class(HTMX_ELEMENT(note), "visually-hidden");
	htmx_node_set_text_content(HTMX_NODE(note), " (opens in a new tab)");
	ADD(link, words);
	ADD(link, note);
	htmx_element_add_class(HTMX_ELEMENT(link), "link-out");
	htmx_a_set_target(link, "_blank");
	htmx_a_set_rel(link, "noreferrer noopener");
	return HTMX_NODE(link);
}

/* A base path as stored: "" or "/" and segments of plain characters. */
static gboolean
base_path_plain(const gchar *path)
{
	const gchar *p;

	if (path == NULL || path[0] == '\0')
		return TRUE;
	if (path[0] != '/' || strstr(path, "//") != NULL || strstr(path, "/.") != NULL)
		return FALSE;
	for (p = path; *p != '\0'; p++) {
		if (!g_ascii_isalnum(*p) && strchr("/-._~", *p) == NULL)
			return FALSE;
	}
	return TRUE;
}

HtmxNode *
axis_web_setup_link_to_service(
	const gchar *address,
	const gchar *base_path,
	const gchar *fixed_path,
	const gchar *text
){
	g_autoptr(GUri) uri = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *href = NULL;
	const gchar *scheme;

	if (address == NULL || !base_path_plain(base_path))
		return NULL;
	uri = g_uri_parse(address, G_URI_FLAGS_NONE, NULL);
	if (uri == NULL)
		return NULL;
	scheme = g_uri_get_scheme(uri);
	/* Only an origin, as the layers store an address: no user, no path, no
	 * query, no fragment. */
	if ((g_strcmp0(scheme, "http") != 0 && g_strcmp0(scheme, "https") != 0) || g_uri_get_host(uri) == NULL ||
	    g_uri_get_host(uri)[0] == '\0' || g_uri_get_userinfo(uri) != NULL || g_uri_get_query(uri) != NULL ||
	    g_uri_get_fragment(uri) != NULL || (g_uri_get_path(uri)[0] != '\0' && strcmp(g_uri_get_path(uri), "/") != 0))
		return NULL;
	path = g_strconcat(base_path != NULL ? base_path : "", fixed_path, NULL);
	href = g_uri_join(G_URI_FLAGS_NONE, scheme, NULL, g_uri_get_host(uri), g_uri_get_port(uri), path, NULL, NULL);
	return axis_web_setup_link_out(href, text);
}

HtmxNode *
axis_web_setup_more(const gchar *summary)
{
	HtmxDetails *details = htmx_details_new();
	HtmxSummary *head = htmx_summary_new();

	htmx_element_add_class(HTMX_ELEMENT(details), "guide-more");
	htmx_node_set_text_content(HTMX_NODE(head), summary);
	ADD(details, head);
	return HTMX_NODE(details);
}

gchar *
axis_web_setup_duration_text(gint64 seconds)
{
	if (seconds <= 0)
		return g_strdup("no time at all");
	if (seconds % 86400 == 0)
		return g_strdup_printf("%" G_GINT64_FORMAT " %s", seconds / 86400, seconds == 86400 ? "day" : "days");
	if (seconds % 3600 == 0)
		return g_strdup_printf("%" G_GINT64_FORMAT " %s", seconds / 3600, seconds == 3600 ? "hour" : "hours");
	if (seconds % 60 == 0)
		return g_strdup_printf("%" G_GINT64_FORMAT " %s", seconds / 60, seconds == 60 ? "minute" : "minutes");
	return g_strdup_printf("%" G_GINT64_FORMAT " seconds", seconds);
}

/* A list of @items, ended by %NULL. */
static HtmxNode *
list_of(
	gboolean            ordered,
	const gchar        *css,
	const gchar *const *items
){
	HtmxNode *list = ordered ? HTMX_NODE(htmx_ol_new()) : HTMX_NODE(htmx_ul_new());
	guint i;

	if (css != NULL)
		htmx_element_add_class(HTMX_ELEMENT(list), css);
	for (i = 0; items[i] != NULL; i++)
		ADD(list, htmx_li_new_with_text(items[i]));
	return list;
}

/* One of the before-and-after mount lists: a caption and its lines. */
static HtmxNode *
layout_block(
	const gchar *caption,
	const gchar *lines,
	const gchar *css
){
	HtmxFigure *figure = htmx_figure_new();
	HtmxFigcaption *text = htmx_figcaption_new();

	htmx_element_add_class(HTMX_ELEMENT(figure), "guide-layout");
	if (css != NULL)
		htmx_element_add_class(HTMX_ELEMENT(figure), css);
	htmx_node_set_text_content(HTMX_NODE(text), caption);
	ADD(figure, text);
	ADD(figure, htmx_pre_new_with_text(lines));
	return HTMX_NODE(figure);
}

/* ── The guides ─────────────────────────────────────────────────── */

HtmxNode *
axis_web_setup_guide_token(void)
{
	HtmxNode *more = axis_web_setup_more("Why the token, and not the key");

	ADD(more, axis_web_setup_text(NULL, "TMDb shows every account two values on its API page. The API Key, 32 "
		"letters and digits, has to travel in the address of each request. The API Read Access Token travels in a "
		"request header instead, which is how Axis sends it, so it is never part of an address that a proxy or a "
		"log could keep."));
	ADD(more, axis_web_setup_text(NULL, "Axis keeps the token as it keeps every secret: it is never shown again, "
		"never written to a log, and sent only to api.themoviedb.org. What it sends besides is the words searched "
		"for or a title's identifier, and its own name and version."));
	return more;
}

HtmxNode *
axis_web_setup_guide_category(void)
{
	HtmxNode *more = axis_web_setup_more("Why Axis wants a category of its own");

	ADD(more, axis_web_setup_text(NULL, "A download client often works for more than one program. Axis looks only at "
		"the downloads that carry its own category (a label, in Deluge), and of those it changes or removes only the "
		"ones it sent itself. A category of its own keeps whatever Radarr, Sonarr or a person added out of its sight "
		"altogether, and keeps Radarr and Sonarr from importing what Axis downloaded, since they too watch only "
		"their own."));
	ADD(more, axis_web_setup_text(NULL, "Axis uses axis unless you choose another; keep it apart from movies, tv, "
		"radarr and sonarr. In SABnzbd, add the category under Config, Categories: a download sent with a category "
		"SABnzbd does not have lands in its default one, where Axis does not look. In Deluge, labels come from its "
		"Label plugin: switch it on under Preferences, Plugins, and Axis makes its label there when it tests the "
		"client. qBittorrent has categories of its own, and Axis makes its category there when it tests the "
		"client."));
	return more;
}

HtmxNode *
axis_web_setup_guide_one_disk(void)
{
	HtmxNode *more = axis_web_setup_more("Why the downloads and the library belong on one disk");
	HtmxDiv *pair = htmx_div_new();

	ADD(more, axis_web_setup_text(NULL, "An import is a move when the finished download and the library folder are on "
		"one disk as Axis sees it: one rename per file, made in an instant and taking no more space, after which "
		"nothing of what was imported is left in the download folder. A torrent left seeding is hardlinked instead: a "
		"second name for the same file, so that it keeps seeding the original."));
	ADD(more, axis_web_setup_text(NULL, "A rename or a hardlink cannot cross from one disk to another, and on Linux "
		"not from one mount to another either, even of the same disk. When the downloads and the library are on "
		"different disks, or reach Axis's container through different mounts, every import is a copy: it takes as "
		"long as copying the file, and as much space again until the download's file is deleted (a torrent left "
		"seeding keeps it its whole life). The usual cause is a container that mounts each folder on its own:"));
	htmx_element_add_class(HTMX_ELEMENT(pair), "guide-layouts");
	ADD(pair, layout_block("Before: each folder mounted on its own. Every import is a copy, though both are on one "
		"disk.", "volumes:\n  - /mnt/disk1/downloads:/downloads\n  - /mnt/disk1/movies:/movies", "guide-layout-before"));
	ADD(pair, layout_block("After: the whole disk mounted once. Imports from its downloads into its library are "
		"renames (or hardlinks, for a torrent left seeding).", "volumes:\n  - /mnt/disk1:/media/disk1",
		"guide-layout-after"));
	ADD(more, pair);
	ADD(more, axis_web_setup_text(NULL, "Then add /media/disk1 as a disk, put the library folder for Movies at movies on it, and "
		"let the download client keep its finished downloads in a folder on the same disk, such as "
		"/mnt/disk1/downloads."));
	return more;
}

HtmxNode *
axis_web_setup_guide_mapping(void)
{
	static const AxisWebColumn columns[] = {
		{ "The client sees", AXIS_WEB_COL_NAME },
		{ "Axis sees", AXIS_WEB_COL_TEXT },
		{ "Mapping to add", AXIS_WEB_COL_WIDE },
		{ NULL, 0 }
	};
	static const struct
	{
		const gchar *client;
		const gchar *axis;
		const gchar *map;
	} layouts[] = {
		{ "The disk, mounted where Axis has it: /media/disk1", "/media/disk1",
		  "Client's path /media/disk1, on the disk, with no folder on it" },
		{ "The disk, mounted elsewhere: /data", "/media/disk1",
		  "Client's path /data, on the disk, with no folder on it" },
		{ "Only its own folder: /downloads, which is /mnt/disk1/downloads", "/media/disk1",
		  "Client's path /downloads, on the disk, at downloads" }
	};
	HtmxNode *more = axis_web_setup_more("What a path mapping is, and the layouts people use");
	HtmxNode *table = axis_web_table_new("setup-layouts", columns);
	HtmxTbody *body = htmx_tbody_new();
	guint i;

	ADD(more, axis_web_setup_text(NULL, "A download client names its files by its own paths, which are its "
		"container's, and those are seldom Axis's. A path mapping tells Axis where a folder of the client's is for "
		"Axis: on which disk, and where on it. Axis never guesses: a finished download whose path no mapping "
		"covers waits, and says so, here and on the client's page."));
	for (i = 0; i < G_N_ELEMENTS(layouts); i++) {
		HtmxTr *row = htmx_tr_new();

		ADD(row, htmx_td_new_with_text(layouts[i].client));
		ADD(row, htmx_td_new_with_text(layouts[i].axis));
		ADD(row, htmx_td_new_with_text(layouts[i].map));
		ADD(body, row);
	}
	ADD(table, body);
	ADD(more, table);
	ADD(more, axis_web_setup_text(NULL, "In each, Axis's container mounts the disk whole at /media/disk1. Whether an import is a rename (or a hardlink) rather than a copy depends only on what Axis sees: "
		"the client's own layout matters for the mapping, not for the move."));
	return more;
}

HtmxNode *
axis_web_setup_guide_writing(gint64 recycle_s)
{
	g_autofree gchar *kept = recycle_s >= 0 ? axis_web_setup_duration_text(recycle_s) : g_strdup("two days");
	g_autofree gchar *recycle = recycle_s == 0 ? g_strdup("When a better release replaces a file, move the old one "
		"into .axis-recycle at the top of its disk and remove it once the upgrade is recorded (Keep files replaced by an "
		"upgrade for, in Settings, under Imports).") : g_strdup_printf("When a better release replaces a file, move the old "
		"one into .axis-recycle at the top of its disk, and remove it %s later (Keep files replaced by an upgrade "
		"for, in Settings, under Imports).", kept);
	const gchar *does[] = {
		"Make the folders a title needs inside the library folder it goes to: a movie's folder, or a series' folder and "
		"its season folders, named as the File naming page shows.",
		"Move each finished download's files there: by a rename when the download is on the library folder's disk, otherwise "
		"by a copy that is read back and compared before it takes its name, the download's file being deleted once "
		"the import is recorded. Subtitles and extras in the usual places come with it. A torrent's client is asked to "
		"let go of the torrent first, since Axis is not a seedbox; a torrent left seeding (After a torrent is imported, "
		"in Settings, under Imports: a private tracker that needs a ratio does) is hardlinked instead, and so is "
		"everything when Files taken from a download is Always hardlink.",
		"Once every wanted file of a download is imported, ask its client to remove it, and delete what the import did "
		"not take (an .nfo, a sample) with the download's folder, inside the client's folder for Axis and only where "
		"strict rules allow it. Anything else left there is listed under Downloads, Leftovers, for you to delete or "
		"not; a cleanup of your own that deletes old things in those folders is safe too.",
		recycle,
		NULL
	};
	static const gchar *const never[] = {
		"Change anything a download client holds that Axis did not send it, or the files a torrent seeds.",
		"Write anywhere but inside the library folders, the .axis-recycle folder at the top of a disk, each disk's "
		".axis-volume marker, and the client's folders for Axis's own downloads (to move their files out and delete "
		"what an import left).",
		"Delete a file of your library on the spot: a file it replaces waits in .axis-recycle first.",
		"Convert, move or rename the files already in your library by itself: a conversion runs only when you start "
		"one or confirm a conversion run.",
		NULL
	};
	HtmxDiv *block = htmx_div_new();
	HtmxDiv *will = htmx_div_new();
	HtmxDiv *wont = htmx_div_new();
	HtmxHeading *will_head = htmx_h3_new();
	HtmxHeading *wont_head = htmx_h3_new();

	htmx_element_add_class(HTMX_ELEMENT(block), "guide-columns");
	htmx_node_set_text_content(HTMX_NODE(will_head), "What Axis does once it may write");
	htmx_node_set_text_content(HTMX_NODE(wont_head), "What Axis never does");
	ADD(will, will_head);
	ADD(will, list_of(FALSE, "guide-list", does));
	ADD(wont, wont_head);
	ADD(wont, list_of(FALSE, "guide-list", never));
	ADD(block, will);
	ADD(block, wont);
	return HTMX_NODE(block);
}
