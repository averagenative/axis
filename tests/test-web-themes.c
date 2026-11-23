/*
 * test-web-themes.c - colour themes, the mark and the icons
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Runs a real HtmxServer with the shell installed (an auth without a
 * database, as test-web.c does) and checks, through HTTP only:
 *
 *  - the stylesheet: axis.css has no colour value of any kind, every
 *    variable it uses is one of the fixed list, and themes.css holds
 *    nothing but the nine theme blocks, each setting exactly that list;
 *  - contrast: every theme's variables, parsed from the served
 *    themes.css, give the WCAG ratios the spec requires for every
 *    required pair (AA; AAA for the body text of "contrast");
 *  - the server: the theme attribute and metas for every theme, the
 *    default, the fallback for an unknown id, one user's choice not
 *    touching another's, and no inline style or script on any page;
 *  - the mark, drawn inline with the geometry of logo-mark.svg;
 *  - the icons: the three well-known paths, every icon a page or the
 *    manifest names, with its stated pixel size, and an opaque touch icon;
 *  - that nothing else became public.
 *
 * Saving a choice needs the database: those tests are in test-web-auth.c
 * (persistence) and test-web-settings.c (the default theme setting).
 */

#include <glib.h>
#include <htmx-glib.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <math.h>
#include <string.h>

#include "axis-test-web.h"
#include "core/axis-module.h"
#include "modules/movies/axis-movies.h"
#include "modules/tv/axis-tv.h"
#include "web/axis-web.h"
#include "web/axis-web-auth.h"
#include "web/axis-web-auth-private.h"
#include "web/axis-web-path.h"
#include "web/axis-web-private.h"
#include "web/axis-web-theme.h"

typedef struct
{
	HtmxServer         *server;
	AxisModuleRegistry *registry;
	AxisWebAuth        *auth;
	AxisWeb            *web;
	SoupSession        *session;
	guint16             port;
	gchar              *cookie;	/* "tester": no theme chosen */
	gchar              *other_cookie;	/* "other": no theme chosen */
} Fixture;

/* ── Fixture ────────────────────────────────────────────────────── */

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	GError *error = NULL;

	memset(fx, 0, sizeof(*fx));
	axis_web_theme_set_default(NULL);
	fx->registry = axis_module_registry_new();
	fx->server = htmx_server_new();
	g_assert_true(htmx_server_listen_on(fx->server, "127.0.0.1", 0, &error));
	g_assert_no_error(error);
	fx->auth = axis_web_auth_new(NULL, "", AXIS_SECURE_COOKIES_AUTO, &error);
	g_assert_no_error(error);
	fx->web = axis_web_new(fx->server, fx->registry, fx->auth);
	g_assert_true(htmx_server_start(fx->server, &error));
	g_assert_no_error(error);
	fx->cookie = axis_web_auth_open_test_session(fx->auth, "tester", AXIS_USER_ROLE_ADMIN);
	fx->other_cookie = axis_web_auth_open_test_session(fx->auth, "other", AXIS_USER_ROLE_USER);
	fx->port = axis_test_web_port(fx->server);
	fx->session = soup_session_new_with_options("timeout", 10, "idle-timeout", 10, NULL);
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	axis_web_theme_set_default(NULL);
	g_object_unref(fx->session);
	htmx_server_stop(fx->server);
	axis_web_free(fx->web);
	axis_web_auth_free(fx->auth);
	g_object_unref(fx->server);
	axis_module_registry_free(fx->registry);
	g_free(fx->cookie);
	g_free(fx->other_cookie);
}

static AxisTestReply
fetch_with(
	Fixture     *fx,
	const gchar *method,
	const gchar *path,
	const gchar *cookie,
	const gchar *form
){
	return axis_test_web_request(fx->session, fx->port, method, path, cookie, NULL, form);
}

static AxisTestReply
fetch(
	Fixture     *fx,
	const gchar *path,
	const gchar *cookie
){
	return fetch_with(fx, "GET", path, cookie, NULL);
}

/* The body of the static asset @asset, fetched without a session. */
static gchar *
asset_body(
	Fixture     *fx,
	const gchar *asset
){
	g_autofree gchar *url = axis_web_asset_url(asset);
	AxisTestReply reply = fetch(fx, url, NULL);
	gchar *body;

	g_assert_cmpuint(reply.status, ==, 200);
	body = g_steal_pointer(&reply.body);
	axis_test_reply_clear(&reply);

	return body;
}

static void
assert_contains(const gchar *haystack, const gchar *needle)
{
	if (strstr(haystack, needle) == NULL)
		g_error("expected to find '%s' in:\n%s", needle, haystack);
}

static void
assert_lacks(const gchar *haystack, const gchar *needle)
{
	if (strstr(haystack, needle) != NULL)
		g_error("did not expect '%s' in:\n%s", needle, haystack);
}

static guint
count_of(
	const gchar *haystack,
	const gchar *needle
){
	guint count = 0;
	const gchar *p = haystack;

	while ((p = strstr(p, needle)) != NULL) {
		count++;
		p += strlen(needle);
	}

	return count;
}

/* The value of attribute @name in @tag (a "<...>" string), or NULL. */
static gchar *
tag_attr(
	const gchar *tag,
	const gchar *name
){
	g_autofree gchar *needle = g_strdup_printf(" %s=\"", name);
	const gchar *start = strstr(tag, needle);
	const gchar *end;

	if (start == NULL)
		return NULL;
	start += strlen(needle);
	end = strchr(start, '"');
	g_assert_nonnull(end);

	return g_strndup(start, end - start);
}

/* The @n-th (from 0) tag that starts with @prefix ("<link", "<meta", "<html"),
 * from its "<" to its ">"; NULL if there is none. */
static gchar *
nth_tag(
	const gchar *html,
	const gchar *prefix,
	guint        n
){
	const gchar *p = html;
	guint seen = 0;

	while ((p = strstr(p, prefix)) != NULL) {
		const gchar *end = strchr(p, '>');

		g_assert_nonnull(end);
		if (seen++ == n)
			return g_strndup(p, end - p + 1);
		p = end;
	}

	return NULL;
}

/* The first tag starting with @prefix whose attribute @name is @value. */
static gchar *
find_tag(
	const gchar *html,
	const gchar *prefix,
	const gchar *name,
	const gchar *value
){
	guint i;

	for (i = 0;; i++) {
		g_autofree gchar *tag = nth_tag(html, prefix, i);
		g_autofree gchar *found = NULL;

		if (tag == NULL)
			return NULL;
		found = tag_attr(tag, name);
		if (g_strcmp0(found, value) == 0)
			return g_steal_pointer(&tag);
	}
}

/* The @n-th tag starting with @prefix whose attribute @name is @value. */
static gchar *
nth_tag_by(
	const gchar *html,
	const gchar *prefix,
	const gchar *name,
	const gchar *value,
	guint        n
){
	guint i;

	for (i = 0;; i++) {
		g_autofree gchar *tag = nth_tag(html, prefix, i);
		g_autofree gchar *found = NULL;

		if (tag == NULL)
			return NULL;
		found = tag_attr(tag, name);
		if (g_strcmp0(found, value) == 0 && n-- == 0)
			return g_steal_pointer(&tag);
	}
}

/* The content (or other @attribute) of the @n-th meta element called
 * @name, or NULL if there is none. */
static gchar *
meta_attr(
	const gchar *html,
	const gchar *name,
	guint        n,
	const gchar *attribute
){
	g_autofree gchar *tag = nth_tag_by(html, "<meta", "name", name, n);

	return tag != NULL ? tag_attr(tag, attribute) : NULL;
}

static gchar *
html_theme(const gchar *html)
{
	g_autofree gchar *tag = nth_tag(html, "<html", 0);

	g_assert_nonnull(tag);
	return tag_attr(tag, "data-theme");
}

/* No inline style, and no script that has a body. */
static void
assert_no_inline_code(const gchar *html)
{
	const gchar *p = html;

	assert_lacks(html, "<style");
	assert_lacks(html, " style=");
	assert_lacks(html, "\tstyle=");
	assert_lacks(html, "\nstyle=");

	while ((p = strstr(p, "<script")) != NULL) {
		const gchar *end = strchr(p, '>');
		g_autofree gchar *tag = NULL;

		g_assert_nonnull(end);
		tag = g_strndup(p, end - p + 1);
		assert_contains(tag, " src=");
		g_assert_true(g_str_has_prefix(end + 1, "</script>"));
		p = end;
	}
}

/* ── Reading the stylesheets ────────────────────────────────────── */

/* The variables every theme sets: the fixed list. */
static const gchar *const colour_vars[] = {
	"bg", "surface", "surface-alt", "code-bg", "border", "control-border",
	"text", "text-muted", "link", "focus", "accent", "accent-text",
	"logo-step-1", "logo-step-2", "logo-step-3", "logo-line",
	"ok", "ok-bg", "warn", "warn-bg", "danger", "danger-bg", "offline", "offline-bg",
	"series-1", "series-2", "series-3", "series-4", "series-5", "series-6",
	NULL
};

/* Variables axis.css may use that are not colours (the phone-* ones are
 * the phone layout's heights and edges). */
static const gchar *const plain_vars[] = {
	"font", "mono", "gutter", "radius", "max-width", "control-height", "control-height-small", "space",
	"label-gap", "phone-line", "phone-row", "phone-tabs", "phone-left", "phone-right", NULL
};

static gboolean
in_list(
	const gchar *const *list,
	const gchar        *name
){
	guint i;

	for (i = 0; list[i] != NULL; i++) {
		if (strcmp(list[i], name) == 0)
			return TRUE;
	}

	return FALSE;
}

static gchar *
strip_comments(const gchar *css)
{
	GString *out = g_string_new(NULL);
	const gchar *p = css;

	while (*p != '\0') {
		if (p[0] == '/' && p[1] == '*') {
			const gchar *end = strstr(p + 2, "*/");

			g_assert_nonnull(end);
			p = end + 2;
			continue;
		}
		g_string_append_c(out, *p++);
	}

	return g_string_free(out, FALSE);
}

/* The CSS named colours (CSS Color Module Level 4), lower case. */
static const gchar *const colour_names[] = {
	"aliceblue", "antiquewhite", "aqua", "aquamarine", "azure", "beige", "bisque", "black",
	"blanchedalmond", "blue", "blueviolet", "brown", "burlywood", "cadetblue", "chartreuse",
	"chocolate", "coral", "cornflowerblue", "cornsilk", "crimson", "cyan", "darkblue",
	"darkcyan", "darkgoldenrod", "darkgray", "darkgreen", "darkgrey", "darkkhaki",
	"darkmagenta", "darkolivegreen", "darkorange", "darkorchid", "darkred", "darksalmon",
	"darkseagreen", "darkslateblue", "darkslategray", "darkslategrey", "darkturquoise",
	"darkviolet", "deeppink", "deepskyblue", "dimgray", "dimgrey", "dodgerblue", "firebrick",
	"floralwhite", "forestgreen", "fuchsia", "gainsboro", "ghostwhite", "gold", "goldenrod",
	"gray", "green", "greenyellow", "grey", "honeydew", "hotpink", "indianred", "indigo",
	"ivory", "khaki", "lavender", "lavenderblush", "lawngreen", "lemonchiffon", "lightblue",
	"lightcoral", "lightcyan", "lightgoldenrodyellow", "lightgray", "lightgreen", "lightgrey",
	"lightpink", "lightsalmon", "lightseagreen", "lightskyblue", "lightslategray",
	"lightslategrey", "lightsteelblue", "lightyellow", "lime", "limegreen", "linen", "magenta",
	"maroon", "mediumaquamarine", "mediumblue", "mediumorchid", "mediumpurple",
	"mediumseagreen", "mediumslateblue", "mediumspringgreen", "mediumturquoise",
	"mediumvioletred", "midnightblue", "mintcream", "mistyrose", "moccasin", "navajowhite",
	"navy", "oldlace", "olive", "olivedrab", "orange", "orangered", "orchid", "palegoldenrod",
	"palegreen", "paleturquoise", "palevioletred", "papayawhip", "peachpuff", "peru", "pink",
	"plum", "powderblue", "purple", "rebeccapurple", "red", "rosybrown", "royalblue",
	"saddlebrown", "salmon", "sandybrown", "seagreen", "seashell", "sienna", "silver",
	"skyblue", "slateblue", "slategray", "slategrey", "snow", "springgreen", "steelblue",
	"tan", "teal", "thistle", "tomato", "turquoise", "violet", "wheat", "white", "whitesmoke",
	"yellow", "yellowgreen",
	NULL
};

static const gchar *const colour_functions[] = {
	"rgb(", "rgba(", "hsl(", "hsla(", "hwb(", "lab(", "lch(", "oklab(", "oklch(", "color(",
	"color-mix(", "light-dark(", NULL
};

/*
 * Why the declaration value @value holds a colour, or NULL.  Colours are
 * a hex literal, a colour function or a colour name; "transparent" and
 * "currentcolor" name no colour and are allowed.
 */
static gchar *
colour_in_value(const gchar *value)
{
	g_autofree gchar *lower = g_ascii_strdown(value, -1);
	const gchar *p;
	guint i;

	for (i = 0; colour_functions[i] != NULL; i++) {
		if (strstr(lower, colour_functions[i]) != NULL)
			return g_strdup_printf("the function %s", colour_functions[i]);
	}

	for (p = strchr(lower, '#'); p != NULL; p = strchr(p + 1, '#')) {
		gsize digits = 0;

		while (g_ascii_isxdigit(p[1 + digits]))
			digits++;
		if (!g_ascii_isalnum(p[1 + digits]) && (digits == 3 || digits == 4 || digits == 6 || digits == 8))
			return g_strdup_printf("a hex colour");
	}

	p = lower;
	while (*p != '\0') {
		GString *word = g_string_new(NULL);
		gboolean named;

		while (*p != '\0' && (g_ascii_isalpha(*p) || *p == '-')) {
			g_string_append_c(word, *p);
			p++;
		}
		named = word->len > 0 && in_list(colour_names, word->str);
		if (named) {
			gchar *why = g_strdup_printf("the colour name '%s'", word->str);

			g_string_free(word, TRUE);
			return why;
		}
		g_string_free(word, TRUE);
		if (*p != '\0' && !g_ascii_isalpha(*p) && *p != '-')
			p++;
	}

	return NULL;
}

/*
 * Calls @check(property, value) for every declaration in @css (comments
 * already removed): a statement ended by ';' or '}' rather than '{'.
 */
typedef void (*DeclFunc)(const gchar *property, const gchar *value, gpointer user_data);

static void
each_declaration(
	const gchar *css,
	DeclFunc     func,
	gpointer     user_data
){
	GString *buffer = g_string_new(NULL);
	const gchar *p;

	for (p = css;; p++) {
		if (*p == '{' || *p == '\0') {
			g_string_truncate(buffer, 0);	/* a selector or an at-rule */
		} else if (*p == ';' || *p == '}') {
			gchar *colon = strchr(buffer->str, ':');

			if (colon != NULL) {
				g_autofree gchar *property = g_strndup(buffer->str, colon - buffer->str);
				g_autofree gchar *value = g_strdup(colon + 1);

				func(g_strstrip(property), g_strstrip(value), user_data);
			}
			g_string_truncate(buffer, 0);
		} else {
			g_string_append_c(buffer, *p);
		}
		if (*p == '\0')
			break;
	}
	g_string_free(buffer, TRUE);
}

static void
check_no_colour(
	const gchar *property,
	const gchar *value,
	gpointer     user_data
){
	g_autofree gchar *why = colour_in_value(value);

	if (why != NULL)
		g_error("axis.css: '%s: %s' holds %s; a colour belongs in themes.css", property, value, why);
}

static void
collect_vars(
	const gchar *property,
	const gchar *value,
	gpointer     user_data
){
	GHashTable *used = user_data;
	const gchar *p = value;

	while ((p = strstr(p, "var(--")) != NULL) {
		gsize length;
		gchar *name;

		p += strlen("var(--");
		length = strcspn(p, ",) ");
		name = g_strndup(p, length);
		g_hash_table_add(used, name);
		p += length;
	}
}

/* One theme block of themes.css. */
typedef struct
{
	gchar      *id;
	gboolean    light_media;	/* inside @media (prefers-color-scheme: light) */
	gchar      *color_scheme;
	GHashTable *vars;		/* name without "--" -> value */
} ThemeBlock;

static void
theme_block_free(ThemeBlock *block)
{
	g_free(block->id);
	g_free(block->color_scheme);
	g_hash_table_unref(block->vars);
	g_free(block);
}

static void
block_declaration(
	const gchar *property,
	const gchar *value,
	gpointer     user_data
){
	ThemeBlock *block = user_data;

	if (g_str_has_prefix(property, "--"))
		g_hash_table_replace(block->vars, g_strdup(property + 2), g_strdup(value));
	else if (strcmp(property, "color-scheme") == 0)
		block->color_scheme = g_strdup(value);
	else
		g_error("themes.css: unexpected property '%s' in theme %s", property, block->id);
}

/* Parses the rules at @p until the closing brace of the enclosing block
 * (or the end).  Only theme blocks and one @media block are accepted. */
static void
parse_rules(
	const gchar **p,
	gboolean      light_media,
	GPtrArray    *blocks
){
	while (TRUE) {
		const gchar *start;
		const gchar *open;
		g_autofree gchar *prelude = NULL;

		while (g_ascii_isspace(**p))
			(*p)++;
		if (**p == '\0')
			return;
		if (**p == '}') {
			(*p)++;
			return;
		}

		start = *p;
		open = strchr(start, '{');
		g_assert_nonnull(open);
		prelude = g_strstrip(g_strndup(start, open - start));
		*p = open + 1;

		if (g_str_has_prefix(prelude, "@media")) {
			g_assert_false(light_media);
			g_assert_cmpstr(prelude, ==, "@media (prefers-color-scheme: light)");
			parse_rules(p, TRUE, blocks);
		} else {
			const gchar *close = strchr(*p, '}');
			g_autofree gchar *body = NULL;
			ThemeBlock *block;
			guint id_length;

			g_assert_nonnull(close);
			/* The default theme also answers a page with no attribute. */
			if (g_str_has_prefix(prelude, ":root:not([data-theme]),")) {
				gchar *rest = g_strstrip(g_strdup(prelude + strlen(":root:not([data-theme]),")));

				g_free(prelude);
				prelude = rest;
			}
			if (!g_str_has_prefix(prelude, "[data-theme=\"") || !g_str_has_suffix(prelude, "\"]"))
				g_error("themes.css holds something other than a theme block: '%s'", prelude);
			id_length = strlen(prelude) - strlen("[data-theme=\"") - strlen("\"]");

			block = g_new0(ThemeBlock, 1);
			block->id = g_strndup(prelude + strlen("[data-theme=\""), id_length);
			block->light_media = light_media;
			block->vars = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
			body = g_strndup(*p, close - *p);
			/* each_declaration() ends a declaration at ';' or '}'. */
			{
				g_autofree gchar *terminated = g_strconcat(body, ";", NULL);

				each_declaration(terminated, block_declaration, block);
			}
			g_ptr_array_add(blocks, block);
			*p = close + 1;
		}
	}
}

static GPtrArray *
parse_themes(const gchar *css)
{
	g_autofree gchar *clean = strip_comments(css);
	const gchar *p = clean;
	GPtrArray *blocks = g_ptr_array_new_with_free_func((GDestroyNotify)theme_block_free);

	parse_rules(&p, FALSE, blocks);

	return blocks;
}

static const ThemeBlock *
block_for(
	GPtrArray   *blocks,
	const gchar *id,
	gboolean     light_media
){
	guint i;

	for (i = 0; i < blocks->len; i++) {
		const ThemeBlock *block = g_ptr_array_index(blocks, i);

		if (strcmp(block->id, id) == 0 && block->light_media == light_media)
			return block;
	}

	return NULL;
}

static const gchar *
var_of(
	const ThemeBlock *block,
	const gchar      *name
){
	const gchar *value = g_hash_table_lookup(block->vars, name);

	if (value == NULL)
		g_error("theme %s does not set --%s", block->id, name);

	return value;
}

/* ── Colours and contrast ───────────────────────────────────────── */

static gdouble
channel(
	const gchar *hex,
	guint        index
){
	gchar pair[3] = { hex[1 + index * 2], hex[2 + index * 2], '\0' };
	gdouble c = g_ascii_strtoull(pair, NULL, 16) / 255.0;

	return c <= 0.03928 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

/* WCAG relative luminance of "#rrggbb". */
static gdouble
luminance(const gchar *hex)
{
	return 0.2126 * channel(hex, 0) + 0.7152 * channel(hex, 1) + 0.0722 * channel(hex, 2);
}

static gdouble
contrast_ratio(
	const gchar *a,
	const gchar *b
){
	gdouble la = luminance(a);
	gdouble lb = luminance(b);

	if (la < lb) {
		gdouble swap = la;

		la = lb;
		lb = swap;
	}

	return (la + 0.05) / (lb + 0.05);
}

static gboolean
is_hex6(const gchar *value)
{
	guint i;

	if (value == NULL || strlen(value) != 7 || value[0] != '#')
		return FALSE;
	for (i = 1; i < 7; i++) {
		if (!g_ascii_isxdigit(value[i]))
			return FALSE;
	}

	return TRUE;
}

static const struct
{
	const gchar *fg;
	const gchar *bg;
	gdouble      minimum;
	const gchar *what;
} pairs[] = {
	{ "text",       "bg",          4.5, "body text on the page" },
	{ "text",       "surface",     4.5, "body text on a card or the header" },
	{ "text",       "surface-alt", 4.5, "body text on a raised part" },
	{ "text",       "code-bg",     4.5, "text in code" },
	{ "text-muted", "bg",          4.5, "muted text on the page" },
	{ "text-muted", "surface",     4.5, "muted text on a card" },
	{ "text-muted", "surface-alt", 4.5, "muted text on a raised part" },
	{ "link",       "bg",          4.5, "a link on the page" },
	{ "link",       "surface",     4.5, "a link on a card" },
	{ "link",       "surface-alt", 4.5, "a link on a raised part" },
	{ "accent-text", "accent",     4.5, "a button label on its background" },
	{ "ok",         "ok-bg",       4.5, "the ok label on its badge" },
	{ "warn",       "warn-bg",     4.5, "the warning label on its badge" },
	{ "danger",     "danger-bg",   4.5, "the failed label on its badge" },
	{ "offline",    "offline-bg",  4.5, "the offline label on its badge" },
	{ "danger",     "bg",          4.5, "error text on the page" },
	{ "danger",     "surface",     4.5, "error text on a card" },
	{ "danger",     "surface-alt", 4.5, "error text on a button" },
	{ "focus",      "bg",          3.0, "the focus ring on the page" },
	{ "focus",      "surface",     3.0, "the focus ring on a card" },
	{ "control-border", "bg",      3.0, "the edge of a control on the page" },
	{ "control-border", "surface", 3.0, "the edge of a control on a card" },
	{ "series-1",   "surface",     3.0, "chart series 1" },
	{ "series-2",   "surface",     3.0, "chart series 2" },
	{ "series-3",   "surface",     3.0, "chart series 3" },
	{ "series-4",   "surface",     3.0, "chart series 4" },
	{ "series-5",   "surface",     3.0, "chart series 5" },
	{ "series-6",   "surface",     3.0, "chart series 6" },
	/*
	 * Every state mark of a film (.film-mark-<state>): its words on its
	 * background (an opaque card colour) and its edge against the card it
	 * sits on.  A mark's words and edge are the same colour but for the two
	 * quiet ones, whose edge is the control edge.
	 */
	{ "link",       "surface",     4.5, "the Searching and Downloading marks' words" },
	{ "link",       "surface",     3.0, "the Searching and Downloading marks' edge on a card" },
	{ "ok",         "surface",     4.5, "the To import and In library marks' words" },
	{ "ok",         "surface",     3.0, "the To import and In library marks' edge on a card" },
	{ "warn",       "surface",     4.5, "the Upgrade wanted mark's words" },
	{ "warn",       "surface",     3.0, "the Upgrade wanted mark's edge on a card" },
	{ "danger",     "surface",     4.5, "the Needs attention mark's words" },
	{ "danger",     "surface",     3.0, "the Needs attention mark's edge on a card" },
	{ "text",       "surface",     4.5, "the Wanted mark's words" },
	{ "control-border", "surface", 3.0, "the Wanted, Not available yet, Not aired and Not monitored marks' edge on a card" },
	{ "text-muted", "surface",     4.5, "the Not available yet, Not aired and Not monitored marks' words" },
	{ NULL, NULL, 0, NULL }
};

/* Every required pair of @block, with the ratio printed when asked. */
static void
check_contrast(const ThemeBlock *block)
{
	gboolean aaa = strcmp(block->id, "contrast") == 0;
	guint i;

	for (i = 0; pairs[i].fg != NULL; i++) {
		gdouble ratio = contrast_ratio(var_of(block, pairs[i].fg), var_of(block, pairs[i].bg));
		gdouble need = pairs[i].minimum;

		/* High contrast meets AAA for the body text. */
		if (aaa && strcmp(pairs[i].fg, "text") == 0 &&
		    (strcmp(pairs[i].bg, "bg") == 0 || strcmp(pairs[i].bg, "surface") == 0 ||
		     strcmp(pairs[i].bg, "surface-alt") == 0))
			need = 7.0;

		if (ratio < need)
			g_error("theme %s: %s: %s on %s is %.2f:1, needs %.1f:1", block->id, pairs[i].what,
				var_of(block, pairs[i].fg), var_of(block, pairs[i].bg), ratio, need);
		if (g_test_verbose())
			g_test_message("%s: %s %.2f", block->id, pairs[i].what, ratio);
	}
}

/* ── The stylesheets ────────────────────────────────────────────── */

static void
test_stylesheet_has_no_colour(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *css = asset_body(fx, "axis.css");
	g_autofree gchar *clean = strip_comments(css);
	g_autoptr(GHashTable) used = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	GHashTableIter iter;
	gpointer key;
	guint i;

	each_declaration(clean, check_no_colour, NULL);

	/* Every variable it reads is on the list ... */
	each_declaration(clean, collect_vars, used);
	g_hash_table_iter_init(&iter, used);
	while (g_hash_table_iter_next(&iter, &key, NULL)) {
		if (!in_list(colour_vars, key) && !in_list(plain_vars, key))
			g_error("axis.css reads var(--%s), which is not on the list of variables", (gchar *)key);
	}
	/* ... and every variable on the list is read somewhere. */
	for (i = 0; colour_vars[i] != NULL; i++) {
		if (!g_hash_table_contains(used, colour_vars[i]))
			g_error("axis.css never reads --%s", colour_vars[i]);
	}
}

static void
test_detector_catches_colours(void)
{
	static const gchar *const bad[] = {
		"#fff", "#FFFFFF", "1px solid #3a1c1c", "rgb(0 0 0)", "rgba(0, 0, 0, 0.5)", "hsl(10 20% 30%)",
		"oklch(0.5 0.1 20)", "color-mix(in srgb, red, blue)", "red", "1px solid WHITE",
		"linear-gradient(black, rebeccapurple)", "#12345678", NULL
	};
	static const gchar *const good[] = {
		"var(--bg)", "1px solid var(--border)", "transparent", "currentcolor", "none", "0.55",
		"system-ui, -apple-system, \"Segoe UI\", Roboto, Arial, sans-serif", "3px", "var(--text-muted)",
		"ui-monospace, Menlo, \"Liberation Mono\", monospace", NULL
	};
	guint i;

	for (i = 0; bad[i] != NULL; i++) {
		g_autofree gchar *why = colour_in_value(bad[i]);

		if (why == NULL)
			g_error("the scan missed a colour in '%s'", bad[i]);
	}
	for (i = 0; good[i] != NULL; i++) {
		g_autofree gchar *why = colour_in_value(good[i]);

		if (why != NULL)
			g_error("the scan flagged '%s' (%s)", good[i], why);
	}
}

static void
test_themes_are_complete(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *css = asset_body(fx, "themes.css");
	g_autoptr(GPtrArray) blocks = parse_themes(css);
	guint i;
	guint j;

	/* One block per theme in the table (plus the auto block's media
	 * override), nothing else. */
	g_assert_cmpuint(blocks->len, ==, axis_web_theme_count() + 1);
	for (i = 0; i < axis_web_theme_count(); i++) {
		const AxisWebTheme *theme = axis_web_theme_at(i);
		const ThemeBlock *block = block_for(blocks, theme->id, FALSE);

		g_assert_nonnull(block);
		g_assert_cmpuint(g_hash_table_size(block->vars), ==, g_strv_length((gchar **)colour_vars));
		for (j = 0; colour_vars[j] != NULL; j++)
			g_assert_true(is_hex6(var_of(block, colour_vars[j])));
		g_assert_cmpstr(block->color_scheme, ==,
			theme->kind == AXIS_WEB_THEME_LIGHT ? "light" : "dark");
	}
	g_assert_nonnull(block_for(blocks, "auto", TRUE));

	/* The setting's choices are the table, in order, and the default is
	 * the first dark one. */
	for (i = 0; i < axis_web_theme_count(); i++)
		g_assert_cmpstr(axis_web_theme_ids[i], ==, axis_web_theme_at(i)->id);
	g_assert_null(axis_web_theme_ids[axis_web_theme_count()]);
	g_assert_cmpstr(axis_web_theme_get_default()->id, ==, "midnight");
	g_assert_cmpint(axis_web_theme_get_default()->kind, ==, AXIS_WEB_THEME_DARK);

	/* Spec: at least five dark themes, at least two light ones (Two light
	 * themes), and a high-contrast one. */
	{
		guint dark = 0;
		guint light = 0;

		for (i = 0; i < axis_web_theme_count(); i++) {
			dark += axis_web_theme_at(i)->kind == AXIS_WEB_THEME_DARK;
			light += axis_web_theme_at(i)->kind == AXIS_WEB_THEME_LIGHT;
		}
		g_assert_cmpuint(dark, >=, 5);
		g_assert_cmpuint(light, >=, 2);
		g_assert_nonnull(axis_web_theme_find("contrast"));
		g_assert_nonnull(axis_web_theme_find("plum"));
		g_assert_nonnull(axis_web_theme_find("linen"));
	}
}

/* ── Colour distance ────────────────────────────────────────────── */

/* "#rrggbb" in CIELAB (D65). */
static void
to_lab(
	const gchar *hex,
	gdouble     *l,
	gdouble     *a,
	gdouble     *b
){
	gdouble rgb[3];
	gdouble x;
	gdouble y;
	gdouble z;
	gdouble f[3];
	guint i;

	for (i = 0; i < 3; i++) {
		gchar pair[3] = { hex[1 + i * 2], hex[2 + i * 2], '\0' };
		gdouble c = g_ascii_strtoull(pair, NULL, 16) / 255.0;

		rgb[i] = c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
	}
	x = (0.4124 * rgb[0] + 0.3576 * rgb[1] + 0.1805 * rgb[2]) / 0.95047;
	y = 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2];
	z = (0.0193 * rgb[0] + 0.1192 * rgb[1] + 0.9505 * rgb[2]) / 1.08883;
	f[0] = x;
	f[1] = y;
	f[2] = z;
	for (i = 0; i < 3; i++)
		f[i] = f[i] > 216.0 / 24389.0 ? pow(f[i], 1.0 / 3.0) : (24389.0 / 27.0 * f[i] + 16.0) / 116.0;
	*l = 116.0 * f[1] - 16.0;
	*a = 500.0 * (f[0] - f[1]);
	*b = 200.0 * (f[1] - f[2]);
}

/* The CIE76 colour difference, Delta E*ab. */
static gdouble
delta_e(
	const gchar *x,
	const gchar *y
){
	gdouble l1, a1, b1, l2, a2, b2;

	to_lab(x, &l1, &a1, &b1);
	to_lab(y, &l2, &a2, &b2);
	return sqrt((l1 - l2) * (l1 - l2) + (a1 - a2) * (a1 - a2) + (b1 - b2) * (b1 - b2));
}

/*
 * The stated distances.  Every two themes' page backgrounds differ by at
 * least a just noticeable difference, side by side (the near-black ones
 * are close by nature: oxblood, midnight's true black, high contrast).
 * The tinted dark themes must carry their hue: a chroma of their own and a
 * clear distance from every other theme.
 */
#define MIN_BACKGROUND_DELTA_E  2.5
#define MIN_TINTED_DELTA_E      6.0
#define MIN_TINTED_CHROMA       8.0

static void
test_delta_e_is_right(void)
{
	gdouble l, a, b;

	/* White is L* 100, black 0; their distance is 100. */
	to_lab("#ffffff", &l, &a, &b);
	g_assert_cmpfloat_with_epsilon(l, 100.0, 0.01);
	g_assert_cmpfloat_with_epsilon(a, 0.0, 0.01);
	g_assert_cmpfloat_with_epsilon(delta_e("#000000", "#ffffff"), 100.0, 0.01);
	/* sRGB red is about L* 53.2, a* 80.1, b* 67.2. */
	to_lab("#ff0000", &l, &a, &b);
	g_assert_cmpfloat_with_epsilon(l, 53.24, 0.05);
	g_assert_cmpfloat_with_epsilon(a, 80.09, 0.1);
	g_assert_cmpfloat_with_epsilon(b, 67.20, 0.1);
}

/* Spec: Distinct backgrounds. */
static void
test_backgrounds_are_distinct(Fixture *fx, gconstpointer data)
{
	static const gchar *const tinted[] = { "dusk", "forest", "ember", "plum", NULL };
	g_autofree gchar *css = asset_body(fx, "themes.css");
	g_autoptr(GPtrArray) blocks = parse_themes(css);
	guint i;
	guint j;

	for (i = 0; i < axis_web_theme_count(); i++) {
		const AxisWebTheme *x = axis_web_theme_at(i);
		const gchar *bg_x;

		if (x->kind == AXIS_WEB_THEME_AUTO)
			continue;
		bg_x = var_of(block_for(blocks, x->id, FALSE), "bg");
		for (j = i + 1; j < axis_web_theme_count(); j++) {
			const AxisWebTheme *y = axis_web_theme_at(j);
			const gchar *bg_y;
			gdouble need = MIN_BACKGROUND_DELTA_E;
			gdouble d;

			if (y->kind == AXIS_WEB_THEME_AUTO)
				continue;
			bg_y = var_of(block_for(blocks, y->id, FALSE), "bg");
			if (in_list(tinted, x->id) || in_list(tinted, y->id))
				need = MIN_TINTED_DELTA_E;
			d = delta_e(bg_x, bg_y);
			if (d < need)
				g_error("the page backgrounds of %s (%s) and %s (%s) differ by %.1f, under %.1f", x->id, bg_x,
					y->id, bg_y, d, need);
			if (g_test_verbose())
				g_test_message("%s / %s: %.1f", x->id, y->id, d);
		}
	}

	for (i = 0; tinted[i] != NULL; i++) {
		gdouble l, a, b;
		const gchar *page = var_of(block_for(blocks, tinted[i], FALSE), "bg");
		const gchar *card = var_of(block_for(blocks, tinted[i], FALSE), "surface");

		to_lab(page, &l, &a, &b);
		if (sqrt(a * a + b * b) < MIN_TINTED_CHROMA)
			g_error("%s: its page (%s) carries too little of its hue", tinted[i], page);
		to_lab(card, &l, &a, &b);
		if (sqrt(a * a + b * b) < MIN_TINTED_CHROMA)
			g_error("%s: its cards (%s) carry too little of its hue", tinted[i], card);
	}
}

static void
test_theme_contrast(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *css = asset_body(fx, "themes.css");
	g_autoptr(GPtrArray) blocks = parse_themes(css);
	guint i;

	for (i = 0; i < blocks->len; i++)
		check_contrast(g_ptr_array_index(blocks, i));
}

static void
test_contrast_code_is_right(void)
{
	/* Known values: black on white is 21:1, and #767676 on white is the
	 * well-known AA limit of 4.54:1. */
	g_assert_cmpfloat_with_epsilon(contrast_ratio("#000000", "#ffffff"), 21.0, 0.001);
	g_assert_cmpfloat_with_epsilon(contrast_ratio("#ffffff", "#000000"), 21.0, 0.001);
	g_assert_cmpfloat_with_epsilon(contrast_ratio("#767676", "#ffffff"), 4.54, 0.01);
	g_assert_cmpfloat_with_epsilon(contrast_ratio("#777777", "#777777"), 1.0, 0.0001);
}

/* Each theme's logo colours deepen away from the line: the further from
 * the line a step is, the closer it is to the background. */
static void
test_logo_colours(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *css = asset_body(fx, "themes.css");
	g_autoptr(GPtrArray) blocks = parse_themes(css);
	guint i;

	for (i = 0; i < blocks->len; i++) {
		const ThemeBlock *block = g_ptr_array_index(blocks, i);
		const gchar *bg = var_of(block, "bg");
		gdouble r1 = contrast_ratio(var_of(block, "logo-step-1"), bg);
		gdouble r2 = contrast_ratio(var_of(block, "logo-step-2"), bg);
		gdouble r3 = contrast_ratio(var_of(block, "logo-step-3"), bg);
		gdouble rl = contrast_ratio(var_of(block, "logo-line"), bg);

		if (!(r1 < r2 && r2 < r3 && r3 < rl))
			g_error("theme %s: the mark's steps %.2f %.2f %.2f and line %.2f do not strengthen "
				"toward the line", block->id, r1, r2, r3, rl);
	}
}

static void
test_oxblood_brand_and_action(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *css = asset_body(fx, "themes.css");
	g_autoptr(GPtrArray) blocks = parse_themes(css);
	const ThemeBlock *oxblood = block_for(blocks, "oxblood", FALSE);
	const gchar *action[] = { "link", "focus", "accent", NULL };
	guint i;

	/* The brand reds, as given. */
	g_assert_cmpstr(var_of(oxblood, "logo-step-1"), ==, "#4a0000");
	g_assert_cmpstr(var_of(oxblood, "logo-step-2"), ==, "#7d0000");
	g_assert_cmpstr(var_of(oxblood, "logo-step-3"), ==, "#b00000");
	g_assert_cmpstr(var_of(oxblood, "logo-line"), ==, "#e30000");

	/* Red is the brand, and teal the only colour of an action: the
	 * action colours are blue-green, never red. */
	for (i = 0; action[i] != NULL; i++) {
		const gchar *hex = var_of(oxblood, action[i]);
		gdouble r = channel(hex, 0);
		gdouble g = channel(hex, 1);
		gdouble b = channel(hex, 2);

		g_assert_cmpfloat(g, >, r);
		g_assert_cmpfloat(b, >, r);
	}
	/* Errors and offline states are not the brand reds. */
	g_assert_cmpstr(var_of(oxblood, "danger"), !=, var_of(oxblood, "logo-line"));
	g_assert_cmpstr(var_of(oxblood, "offline"), !=, var_of(oxblood, "logo-step-3"));
}

/* "Match my device": the dark values are oxblood's, and a media query for
 * a light device sets linen's, its light counterpart. */
static void
test_auto_follows_the_device(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *css = asset_body(fx, "themes.css");
	g_autoptr(GPtrArray) blocks = parse_themes(css);
	const ThemeBlock *dark = block_for(blocks, "auto", FALSE);
	const ThemeBlock *light = block_for(blocks, "auto", TRUE);
	const ThemeBlock *oxblood = block_for(blocks, "oxblood", FALSE);
	const ThemeBlock *linen = block_for(blocks, "linen", FALSE);
	guint i;

	g_assert_nonnull(dark);
	g_assert_nonnull(light);
	g_assert_cmpstr(dark->color_scheme, ==, "dark");
	g_assert_cmpstr(light->color_scheme, ==, "light");
	for (i = 0; colour_vars[i] != NULL; i++) {
		g_assert_cmpstr(var_of(dark, colour_vars[i]), ==, var_of(oxblood, colour_vars[i]));
		g_assert_cmpstr(var_of(light, colour_vars[i]), ==, var_of(linen, colour_vars[i]));
	}
	assert_contains(css, "@media (prefers-color-scheme: light)");
}

/* Linen is oxblood's light counterpart: the same separation of the red
 * brand from the action colour. */
static void
test_linen_brand_and_action(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *css = asset_body(fx, "themes.css");
	g_autoptr(GPtrArray) blocks = parse_themes(css);
	const ThemeBlock *linen = block_for(blocks, "linen", FALSE);
	const gchar *action[] = { "link", "focus", "accent", NULL };
	const gchar *brand[] = { "logo-step-2", "logo-step-3", "logo-line", NULL };
	guint i;

	for (i = 0; action[i] != NULL; i++) {
		const gchar *hex = var_of(linen, action[i]);

		g_assert_cmpfloat(channel(hex, 1), >, channel(hex, 0));
		g_assert_cmpfloat(channel(hex, 2), >, channel(hex, 0));
	}
	for (i = 0; brand[i] != NULL; i++) {
		const gchar *hex = var_of(linen, brand[i]);

		g_assert_cmpfloat(channel(hex, 0), >, channel(hex, 1));
		g_assert_cmpfloat(channel(hex, 0), >, channel(hex, 2));
	}
}

/* ── The stylesheet's measures ──────────────────────────────────── */

/* The values of @property in every rule whose selector is exactly
 * @selector, in rem (other units are a failure). */
static GArray *
rule_rems(
	const gchar *css,
	const gchar *selector,
	const gchar *property
){
	GArray *values = g_array_new(FALSE, FALSE, sizeof(gdouble));
	g_autofree gchar *needle = g_strdup_printf("%s {", selector);
	g_autofree gchar *prop = g_strdup_printf("%s:", property);
	const gchar *p = css;

	while ((p = strstr(p, needle)) != NULL) {
		const gchar *close = strchr(p, '}');
		const gchar *at;

		/* Only a whole selector: the start of the file or of a line. */
		if (p != css && p[-1] != '\n' && p[-1] != '\t') {
			p++;
			continue;
		}
		g_assert_nonnull(close);
		at = g_strstr_len(p, close - p, prop);
		if (at != NULL) {
			gchar *end = NULL;
			gdouble value = g_ascii_strtod(at + strlen(prop), &end);

			g_assert_true(g_str_has_prefix(end, "rem"));
			g_array_append_val(values, value);
		}
		p = close;
	}
	return values;
}

static gdouble
array_min(GArray *values)
{
	gdouble m = G_MAXDOUBLE;
	guint i;

	for (i = 0; i < values->len; i++)
		m = MIN(m, g_array_index(values, gdouble, i));
	return m;
}

static gdouble
array_max(GArray *values)
{
	gdouble m = 0;
	guint i;

	for (i = 0; i < values->len; i++)
		m = MAX(m, g_array_index(values, gdouble, i));
	return m;
}

/* Spec: Brand size.  The name is set larger than any navigation label, and
 * the mark is at least as tall as the name, at every width. */
static void
test_brand_leads(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *css = asset_body(fx, "axis.css");
	g_autofree gchar *clean = strip_comments(css);
	g_autoptr(GArray) name = rule_rems(clean, ".brand-name", "font-size");
	g_autoptr(GArray) tabs = rule_rems(clean, ".tab-list a", "font-size");
	g_autoptr(GArray) pages = rule_rems(clean, ".page-list a", "font-size");
	g_autoptr(GArray) mark = rule_rems(clean, ".brand .logo", "height");

	g_assert_cmpuint(name->len, >=, 1);
	g_assert_cmpuint(tabs->len, >=, 1);
	g_assert_cmpuint(pages->len, >=, 1);
	g_assert_cmpuint(mark->len, >=, 1);
	g_assert_cmpfloat(array_min(name), >, array_max(tabs));
	g_assert_cmpfloat(array_min(name), >, array_max(pages));
	g_assert_cmpfloat(array_min(mark), >=, array_max(name));
	/* The second row is quieter than the first, wherever both are rows of
	 * the header: not on a phone, where the tabs are the bar at the foot of
	 * the screen, its labels small as a bar of tabs has them (the phone
	 * section, from its variables to the end). */
	{
		const gchar *phone = strstr(clean, "--phone-line:");
		g_autofree gchar *before = NULL;
		g_autoptr(GArray) header_tabs = NULL;
		g_autoptr(GArray) header_pages = NULL;

		g_assert_nonnull(phone);
		before = g_strndup(clean, phone - clean);
		g_assert_nonnull(g_strrstr(before, "@media (max-width: 40rem) {"));
		*g_strrstr(before, "@media (max-width: 40rem) {") = '\0';
		header_tabs = rule_rems(before, ".tab-list a", "font-size");
		header_pages = rule_rems(before, ".page-list a", "font-size");
		g_assert_cmpuint(header_tabs->len, >=, 1);
		g_assert_cmpuint(header_pages->len, >=, 1);
		g_assert_cmpfloat(array_max(header_pages), <, array_min(header_tabs));
	}
}

/* Spec: Button beside a labelled field.  Fields, drop-downs and buttons
 * have one height and box; the dense places one smaller height. */
static void
test_one_control_height(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *css = asset_body(fx, "axis.css");
	g_autofree gchar *clean = strip_comments(css);
	const gchar *rule = strstr(clean, "input[type=\"text\"],\ninput[type=\"password\"],\ninput[type=\"number\"],\n"
		"input[type=\"time\"],\nselect,\nbutton {");
	const gchar *small = strstr(clean, ".button-small,\n.data-table button,\n.data-table input[type=\"text\"],\n"
		".data-table input[type=\"password\"],\n.data-table select,\n.filters input[type=\"text\"],\n"
		".filters select,\n.filters button,");
	g_autofree gchar *body = NULL;
	g_autofree gchar *small_body = NULL;

	g_assert_nonnull(rule);
	body = g_strndup(rule, strchr(rule, '}') - rule);
	assert_contains(body, "height: var(--control-height);");
	assert_contains(body, "border: 1px solid var(--control-border);");
	assert_contains(body, "border-radius: var(--radius);");
	assert_contains(body, "line-height: 1.25;");
	g_assert_nonnull(small);
	small_body = g_strndup(small, strchr(small, '}') - small);
	assert_contains(small_body, "height: var(--control-height-small);");
	/* A row of controls shares a bottom edge: the labels' line, the
	 * controls' line and the help's line are the row's own. */
	assert_contains(clean, ".inline-form > .field {\n\tgrid-row: 1 / span 3;\n\tgrid-template-rows: subgrid;");
	assert_contains(clean, ".filters {\n\tdisplay: flex;\n\tflex-wrap: wrap;\n\talign-items: flex-end;");
}

/* The widths the table builder counts with are the stylesheet's. */
static void
test_column_widths_agree(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *css = asset_body(fx, "axis.css");
	g_autofree gchar *clean = strip_comments(css);
	guint role;
	guint n;

	for (role = 0; role < AXIS_WEB_COL_ROLE_COUNT; role++) {
		g_autofree gchar *selector = g_strdup_printf("col.%s", axis_web_column_role_class(role));
		g_autoptr(GArray) widths = rule_rems(clean, selector, "width");
		guint want = axis_web_column_role_width(role);

		if (want == 0) {
			g_assert_cmpuint(widths->len, ==, 0);
			continue;
		}
		g_assert_cmpuint(widths->len, ==, 1);
		g_assert_cmpfloat_with_epsilon(g_array_index(widths, gdouble, 0), want / 100.0, 0.001);
	}
	/* And every least width a table may be given has its rule. */
	for (n = 24; n <= 96; n++) {
		g_autofree gchar *rule = g_strdup_printf(".data-table.min-%u { min-width: %urem; }", n, n);

		assert_contains(clean, rule);
	}
	/* The wider least widths for names and details are the phone's alone
	 * (they come after the rules above, inside a media query for a screen at
	 * most 40rem wide), so that nothing is laid out differently on a wider
	 * one. */
	{
		const gchar *phone = strstr(clean, "@media (max-width: 40rem) {\n\t.data-table.least-25 ");
		const gchar *last = strstr(clean, "\t.data-table.least-96 { min-width: 96rem; }\n}\n");

		g_assert_nonnull(phone);
		g_assert_nonnull(last);
		g_assert_cmpint(strstr(clean, ".data-table.min-96 { min-width: 96rem; }") - clean, <, phone - clean);
		for (n = 25; n <= 96; n++) {
			g_autofree gchar *rule = g_strdup_printf("\t.data-table.least-%u { min-width: %urem; }\n", n, n);
			const gchar *at = strstr(phone, rule);

			g_assert_nonnull(at);
			g_assert_cmpint(at - phone, <, last - phone + 1);
		}
		g_assert_null(strstr(clean, ".data-table.least-24 "));
	}
	/* Tables lay out from their columns, not their contents. */
	assert_contains(clean, ".data-table {\n\twidth: 100%;\n\ttable-layout: fixed;");
}

/* The variable a rule of axis.css gives to @property, as "text-muted" for
 * "color: var(--text-muted);", in the rule whose selectors include
 * @selector; %NULL when it has none. */
static gchar *
rule_variable(
	const gchar *css,
	const gchar *selector,
	const gchar *property
){
	g_autofree gchar *needle = g_strdup_printf("%s", selector);
	g_autofree gchar *prop = g_strdup_printf("\t%s: var(--", property);
	const gchar *p = css;

	while ((p = strstr(p, needle)) != NULL) {
		const gchar *open = strchr(p, '{');
		const gchar *close = open != NULL ? strchr(open, '}') : NULL;
		const gchar *at;
		const gchar *end;

		/* The selector must be followed by a comma or the brace, not by more
		 * of a longer name. */
		if (p[strlen(needle)] != ',' && p[strlen(needle)] != ' ' && p[strlen(needle)] != '\n') {
			p++;
			continue;
		}
		g_assert_nonnull(close);
		at = g_strstr_len(open, close - open, prop);
		if (at == NULL) {
			p = close;
			continue;
		}
		at += strlen(prop);
		end = strchr(at, ')');
		g_assert_nonnull(end);
		return g_strndup(at, end - at);
	}

	return NULL;
}

/* Every state a film can be in has a mark in axis.css, and the colours it
 * takes are the ones the contrast pairs above check, so that "Not out" and
 * the others hold in every theme. */
static void
test_every_film_mark_is_styled(Fixture *fx, gconstpointer data)
{
	static const struct
	{
		const gchar *state;
		const gchar *words;
		const gchar *edge;
	} marks[] = {
		{ "searching", "link", "link" },
		{ "downloading", "link", "link" },
		{ "to_import", "ok", "ok" },
		{ "in_library", "ok", "ok" },
		{ "upgrade_wanted", "warn", "warn" },
		{ "attention", "danger", "danger" },
		{ "wanted", "text", "control-border" },
		{ "not_available", "text-muted", "control-border" },
		{ NULL, NULL, NULL }
	};
	g_autofree gchar *css = asset_body(fx, "axis.css");
	g_autofree gchar *clean = strip_comments(css);
	guint i;

	g_assert_cmpuint(G_N_ELEMENTS(marks) - 1, ==, AXIS_MOVIE_N_STATES);
	for (i = 0; marks[i].state != NULL; i++) {
		g_autofree gchar *selector = g_strdup_printf(".film-mark-%s", marks[i].state);
		g_autofree gchar *words = rule_variable(clean, selector, "color");
		g_autofree gchar *edge = rule_variable(clean, selector, "border-color");

		g_assert_cmpstr(words, ==, marks[i].words);
		g_assert_cmpstr(edge, ==, marks[i].edge);
	}
	/* "Not out" is never taken for "Wanted" (refine-indexer-followups 7.1):
	 * the same kind of edge colour, but a dashed outline with slanted words,
	 * from the same theme variables and no colour of its own. */
	assert_contains(clean, ".film-mark-not_available,\n.film-mark-not_aired {\n\tborder-style: dashed;\n"
		"\tfont-style: italic;\n}");
	g_assert_null(strstr(clean, ".film-mark-wanted {\n\tborder-style"));
	/* And the states the marks above list are the states there are. */
	for (i = 0; i < AXIS_MOVIE_N_STATES; i++) {
		const gchar *name = axis_movie_state_name((AxisMovieState)i);
		guint k;
		gboolean found = FALSE;

		for (k = 0; marks[k].state != NULL; k++)
			found = found || strcmp(marks[k].state, name) == 0;
		g_assert_true(found);
	}
}

/* An episode's states take the same marks (.film-mark-<state>), with the
 * same colours: its two of its own (not aired, not monitored) are quiet, as
 * "Not out" is. */
static void
test_every_episode_mark_is_styled(Fixture *fx, gconstpointer data)
{
	static const struct
	{
		const gchar *state;
		const gchar *words;
		const gchar *edge;
	} marks[] = {
		{ "not_aired", "text-muted", "control-border" },
		{ "wanted", "text", "control-border" },
		{ "searching", "link", "link" },
		{ "downloading", "link", "link" },
		{ "to_import", "ok", "ok" },
		{ "in_library", "ok", "ok" },
		{ "upgrade_wanted", "warn", "warn" },
		{ "attention", "danger", "danger" },
		{ "unmonitored", "text-muted", "control-border" },
		{ NULL, NULL, NULL }
	};
	g_autofree gchar *css = asset_body(fx, "axis.css");
	g_autofree gchar *clean = strip_comments(css);
	guint i;

	g_assert_cmpuint(G_N_ELEMENTS(marks) - 1, ==, AXIS_TV_N_STATES);
	for (i = 0; marks[i].state != NULL; i++) {
		g_autofree gchar *selector = g_strdup_printf(".film-mark-%s", marks[i].state);
		g_autofree gchar *words = rule_variable(clean, selector, "color");
		g_autofree gchar *edge = rule_variable(clean, selector, "border-color");

		g_assert_cmpstr(marks[i].state, ==, axis_tv_state_name((AxisTvState)i));
		g_assert_cmpstr(words, ==, marks[i].words);
		g_assert_cmpstr(edge, ==, marks[i].edge);
	}
}

/* The tables here and the stylesheet agree on the header colour, which is
 * what the browser's own bar is told to be. */
static void
test_theme_color_matches_stylesheet(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *css = asset_body(fx, "themes.css");
	g_autoptr(GPtrArray) blocks = parse_themes(css);
	guint i;

	for (i = 0; i < axis_web_theme_count(); i++) {
		const AxisWebTheme *theme = axis_web_theme_at(i);

		g_assert_cmpstr(theme->theme_color, ==, var_of(block_for(blocks, theme->id, FALSE), "surface"));
		if (theme->kind == AXIS_WEB_THEME_AUTO)
			g_assert_cmpstr(theme->theme_color_light, ==, var_of(block_for(blocks, "auto", TRUE), "surface"));
		else
			g_assert_null(theme->theme_color_light);
	}
}

/* ── The page ───────────────────────────────────────────────────── */

static void
test_default_is_dark_for_everyone(Fixture *fx, gconstpointer data)
{
	AxisTestReply user = fetch(fx, "/", fx->cookie);
	AxisTestReply login = fetch(fx, "/login", NULL);
	g_autofree gchar *user_theme = html_theme(user.body);
	g_autofree gchar *login_theme = html_theme(login.body);
	g_autofree gchar *user_scheme = meta_attr(user.body, "color-scheme", 0, "content");
	g_autofree gchar *login_scheme = meta_attr(login.body, "color-scheme", 0, "content");

	g_assert_cmpuint(user.status, ==, 200);
	g_assert_cmpuint(login.status, ==, 200);
	g_assert_cmpstr(user_theme, ==, "midnight");
	g_assert_cmpstr(login_theme, ==, "midnight");
	g_assert_cmpstr(user_scheme, ==, "dark");
	g_assert_cmpstr(login_scheme, ==, "dark");

	axis_test_reply_clear(&user);
	axis_test_reply_clear(&login);
}

static void
test_every_theme_renders(Fixture *fx, gconstpointer data)
{
	static const gchar *const paths[] = { "/", "/account", "/missing", NULL };
	guint i;
	guint j;

	for (i = 0; i < axis_web_theme_count(); i++) {
		const AxisWebTheme *theme = axis_web_theme_at(i);

		axis_web_auth_set_test_theme(fx->auth, "tester", theme->id);
		for (j = 0; paths[j] != NULL; j++) {
			AxisTestReply reply = fetch(fx, paths[j], fx->cookie);
			g_autofree gchar *id = html_theme(reply.body);
			g_autofree gchar *scheme = meta_attr(reply.body, "color-scheme", 0, "content");
			g_autofree gchar *first = meta_attr(reply.body, "theme-color", 0, "content");
			g_autofree gchar *second = meta_attr(reply.body, "theme-color", 1, "content");
			g_autofree gchar *first_media = meta_attr(reply.body, "theme-color", 0, "media");
			g_autofree gchar *second_media = meta_attr(reply.body, "theme-color", 1, "media");

			g_assert_cmpstr(id, ==, theme->id);
			g_assert_cmpstr(scheme, ==, theme->color_scheme);
			g_assert_cmpstr(first, ==, theme->theme_color);
			if (theme->kind == AXIS_WEB_THEME_AUTO) {
				g_assert_cmpstr(first_media, ==, "(prefers-color-scheme: dark)");
				g_assert_cmpstr(second_media, ==, "(prefers-color-scheme: light)");
				g_assert_cmpstr(second, ==, theme->theme_color_light);
			} else {
				g_assert_null(first_media);
				g_assert_null(second);
			}
			assert_no_inline_code(reply.body);
			axis_test_reply_clear(&reply);
		}
	}
}

static void
test_unknown_theme_falls_back(Fixture *fx, gconstpointer data)
{
	AxisTestReply page;
	g_autofree gchar *id = NULL;

	axis_web_auth_set_test_theme(fx->auth, "tester", "retired-theme");
	page = fetch(fx, "/", fx->cookie);
	id = html_theme(page.body);
	g_assert_cmpstr(id, ==, "midnight");
	axis_test_reply_clear(&page);

	/* ... the default, whichever it is. */
	axis_web_theme_set_default("paper");
	page = fetch(fx, "/", fx->cookie);
	g_free(id);
	id = html_theme(page.body);
	g_assert_cmpstr(id, ==, "paper");
	axis_test_reply_clear(&page);

	/* A default this version does not have is the fallback. */
	axis_web_theme_set_default("no-such-theme");
	g_assert_cmpstr(axis_web_theme_get_default()->id, ==, "midnight");
	g_assert_cmpstr(axis_web_theme_resolve("no-such-theme")->id, ==, "midnight");
	g_assert_cmpstr(axis_web_theme_resolve(NULL)->id, ==, "midnight");
	g_assert_cmpstr(axis_web_theme_resolve("dusk")->id, ==, "dusk");
}

/* A visitor who is not signed in gets Midnight on the login page. */
static void
test_unsigned_visitor_gets_midnight(Fixture *fx, gconstpointer data)
{
	AxisTestReply login = fetch(fx, "/login", NULL);
	g_autofree gchar *id = html_theme(login.body);

	g_assert_cmpuint(login.status, ==, 200);
	g_assert_cmpstr(id, ==, "midnight");
	g_assert_cmpstr(axis_web_theme_resolve(NULL)->id, ==, "midnight");
	axis_test_reply_clear(&login);
}

/* A stored theme that is empty or that no version provides reads as the
 * default, Midnight. */
static void
test_unknown_or_empty_stored_theme_gets_midnight(Fixture *fx, gconstpointer data)
{
	static const gchar *const stored[] = { "", "no-such-theme", NULL };
	guint i;

	for (i = 0; stored[i] != NULL; i++) {
		AxisTestReply page;
		g_autofree gchar *id = NULL;

		g_assert_cmpstr(axis_web_theme_resolve(stored[i])->id, ==, "midnight");
		axis_web_auth_set_test_theme(fx->auth, "tester", stored[i]);
		page = fetch(fx, "/", fx->cookie);
		id = html_theme(page.body);
		g_assert_cmpstr(id, ==, "midnight");
		axis_test_reply_clear(&page);
	}
}

/* With no data-theme attribute the stylesheet gives Midnight's values. */
static void
test_stylesheet_without_attribute_is_midnight(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *css = asset_body(fx, "themes.css");

	g_assert_nonnull(strstr(css, ":root:not([data-theme]),"));
	g_assert_nonnull(strstr(css, "[data-theme=\"midnight\"] {"));
}

static void
test_users_themes_are_separate(Fixture *fx, gconstpointer data)
{
	AxisTestReply mine;
	AxisTestReply theirs;
	AxisTestReply login;
	g_autofree gchar *mine_id = NULL;
	g_autofree gchar *theirs_id = NULL;
	g_autofree gchar *login_id = NULL;

	axis_web_auth_set_test_theme(fx->auth, "tester", "dusk");
	mine = fetch(fx, "/", fx->cookie);
	theirs = fetch(fx, "/", fx->other_cookie);
	login = fetch(fx, "/login", NULL);
	mine_id = html_theme(mine.body);
	theirs_id = html_theme(theirs.body);
	login_id = html_theme(login.body);

	g_assert_cmpstr(mine_id, ==, "dusk");
	g_assert_cmpstr(theirs_id, ==, "midnight");
	g_assert_cmpstr(login_id, ==, "midnight");

	axis_test_reply_clear(&mine);
	axis_test_reply_clear(&theirs);
	axis_test_reply_clear(&login);
}

static void
test_default_theme_applies_to_those_without_a_choice(Fixture *fx, gconstpointer data)
{
	AxisTestReply chosen;
	AxisTestReply plain;
	AxisTestReply login;
	g_autofree gchar *chosen_id = NULL;
	g_autofree gchar *plain_id = NULL;
	g_autofree gchar *login_id = NULL;
	g_autofree gchar *login_scheme = NULL;

	axis_web_auth_set_test_theme(fx->auth, "tester", "forest");
	axis_web_theme_set_default("paper");
	chosen = fetch(fx, "/", fx->cookie);
	plain = fetch(fx, "/", fx->other_cookie);
	login = fetch(fx, "/login", NULL);
	chosen_id = html_theme(chosen.body);
	plain_id = html_theme(plain.body);
	login_id = html_theme(login.body);

	g_assert_cmpstr(chosen_id, ==, "forest");
	g_assert_cmpstr(plain_id, ==, "paper");
	g_assert_cmpstr(login_id, ==, "paper");
	login_scheme = meta_attr(login.body, "color-scheme", 0, "content");
	g_assert_cmpstr(login_scheme, ==, "light");

	axis_test_reply_clear(&chosen);
	axis_test_reply_clear(&plain);
	axis_test_reply_clear(&login);
}

/* ── The mark ───────────────────────────────────────────────────── */

/* The d="..." values of the paths in @svg, in order. */
static GPtrArray *
path_data(const gchar *svg)
{
	GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
	const gchar *p = svg;

	while ((p = strstr(p, "<path")) != NULL) {
		const gchar *end = strchr(p, '>');
		g_autofree gchar *tag = NULL;

		g_assert_nonnull(end);
		tag = g_strndup(p, end - p + 1);
		g_ptr_array_add(paths, tag_attr(tag, "d"));
		p = end;
	}

	return paths;
}

static void
test_mark_is_drawn_inline(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *source = asset_body(fx, "icons/logo-mark.svg");
	g_autoptr(GPtrArray) shapes = path_data(source);
	AxisTestReply page = fetch(fx, "/", fx->cookie);
	AxisTestReply login = fetch(fx, "/login", NULL);
	guint i;

	/* Six wedges and the line, with the geometry of the source file. */
	g_assert_cmpuint(shapes->len, ==, 7);
	for (i = 0; i < shapes->len; i++) {
		g_autofree gchar *needle = g_strdup_printf(" d=\"%s\"", (gchar *)g_ptr_array_index(shapes, i));

		assert_contains(page.body, needle);
	}
	assert_contains(page.body, "viewBox=\"0 0 64 64\"");
	assert_contains(page.body, "<circle");
	assert_contains(page.body, " r=\"3.6\"");
	assert_contains(page.body, "mask=\"url(#axis-hub-header)\"");
	assert_contains(page.body, "id=\"axis-hub-header\"");
	g_assert_cmpuint(count_of(page.body, "logo-step-1"), ==, 2);
	g_assert_cmpuint(count_of(page.body, "logo-step-2"), ==, 2);
	g_assert_cmpuint(count_of(page.body, "logo-step-3"), ==, 2);
	g_assert_cmpuint(count_of(page.body, "logo-line"), ==, 1);
	/* The mark's colours come from the theme: no colour in its markup
	 * apart from the mask's own white and black. */
	assert_lacks(page.body, "stroke=\"#");
	assert_lacks(page.body, "fill=\"#");
	assert_no_inline_code(page.body);
	/* The brand link carries the mark and the name. */
	assert_contains(page.body, "class=\"brand\"");

	/* Login: the mark in the header and, large, with the name; the two
	 * masks have different ids. */
	assert_contains(login.body, "class=\"lockup\"");
	assert_contains(login.body, "lockup-name");
	assert_contains(login.body, "id=\"axis-hub-header\"");
	assert_contains(login.body, "id=\"axis-hub-lockup\"");
	g_assert_cmpuint(count_of(login.body, "<svg"), ==, 2);
	assert_no_inline_code(login.body);

	axis_test_reply_clear(&page);
	axis_test_reply_clear(&login);
}

/* ── Icons ──────────────────────────────────────────────────────── */

typedef struct
{
	guint width;
	guint height;
	guint colour_type;
} PngInfo;

static guint
be32(const guint8 *p)
{
	return ((guint)p[0] << 24) | ((guint)p[1] << 16) | ((guint)p[2] << 8) | (guint)p[3];
}

/* Reads the IHDR of a PNG; fails the test if it is not one. */
static PngInfo
png_info(const gchar *body, gsize length)
{
	static const guint8 signature[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
	const guint8 *b = (const guint8 *)body;
	PngInfo info;

	g_assert_cmpuint(length, >, 33);
	g_assert_cmpint(memcmp(b, signature, 8), ==, 0);
	g_assert_cmpuint(be32(b + 8), ==, 13);
	g_assert_cmpint(memcmp(b + 12, "IHDR", 4), ==, 0);
	info.width = be32(b + 16);
	info.height = be32(b + 20);
	info.colour_type = b[25];

	return info;
}

static void
assert_png(
	AxisTestReply *reply,
	guint          width,
	guint          height
){
	PngInfo info;

	g_assert_cmpuint(reply->status, ==, 200);
	g_assert_cmpstr(axis_test_reply_header(reply, "Content-Type"), ==, "image/png");
	info = png_info(reply->body, reply->length);
	g_assert_cmpuint(info.width, ==, width);
	g_assert_cmpuint(info.height, ==, height);
}

/* "WxH" as two numbers; FALSE if @sizes is not that. */
static gboolean
parse_sizes(
	const gchar *sizes,
	guint       *width,
	guint       *height
){
	gchar *x;
	gchar *end;

	if (sizes == NULL)
		return FALSE;
	*width = (guint)g_ascii_strtoull(sizes, &x, 10);
	if (*x != 'x')
		return FALSE;
	*height = (guint)g_ascii_strtoull(x + 1, &end, 10);

	return *end == '\0';
}

static void
test_well_known_icons(Fixture *fx, gconstpointer data)
{
	static const gchar *const paths[] = {
		"/favicon.ico", "/apple-touch-icon.png", "/apple-touch-icon-precomposed.png", NULL
	};
	g_autofree gchar *ico_url = axis_web_asset_url("icons/favicon.ico");
	g_autofree gchar *touch_url = axis_web_asset_url("icons/apple-touch-icon.png");
	AxisTestReply ico = fetch(fx, ico_url, NULL);
	AxisTestReply touch = fetch(fx, touch_url, NULL);
	guint i;

	g_assert_cmpuint(ico.status, ==, 200);
	g_assert_cmpuint(touch.status, ==, 200);
	for (i = 0; paths[i] != NULL; i++) {
		AxisTestReply reply = fetch(fx, paths[i], NULL);
		AxisTestReply again;
		const gchar *etag;

		/* Without a session, and not by redirect. */
		g_assert_cmpuint(reply.status, ==, 200);
		g_assert_null(axis_test_reply_header(&reply, "Location"));
		g_assert_null(axis_test_reply_header(&reply, "Set-Cookie"));
		if (i == 0) {
			g_assert_cmpstr(axis_test_reply_header(&reply, "Content-Type"), ==, "image/x-icon");
			/* The very file the versioned path serves. */
			g_assert_cmpuint(reply.length, ==, ico.length);
			g_assert_cmpint(memcmp(reply.body, ico.body, ico.length), ==, 0);
		} else {
			assert_png(&reply, 180, 180);
			g_assert_cmpuint(reply.length, ==, touch.length);
			g_assert_cmpint(memcmp(reply.body, touch.body, touch.length), ==, 0);
		}
		/* No version in the URL, so a day, not for ever. */
		g_assert_cmpstr(axis_test_reply_header(&reply, "Cache-Control"), ==, "public, max-age=86400");

		/* A client that has it is told so. */
		etag = axis_test_reply_header(&reply, "ETag");
		g_assert_nonnull(etag);
		{
			const gchar *extra[] = { "If-None-Match", etag, NULL };

			again = axis_test_web_request(fx->session, fx->port, "GET", paths[i], NULL, extra, NULL);
			g_assert_cmpuint(again.status, ==, 304);
			axis_test_reply_clear(&again);
		}
		axis_test_reply_clear(&reply);
	}

	axis_test_reply_clear(&ico);
	axis_test_reply_clear(&touch);
}

static void
test_touch_icon_is_opaque(Fixture *fx, gconstpointer data)
{
	AxisTestReply reply = fetch(fx, "/apple-touch-icon.png", NULL);
	PngInfo info = png_info(reply.body, reply.length);

	/* 180 by 180, and colour type 2 (truecolour): a PNG without an alpha
	 * channel has no transparent pixel. */
	g_assert_cmpuint(info.width, ==, 180);
	g_assert_cmpuint(info.height, ==, 180);
	g_assert_cmpuint(info.colour_type, ==, 2);

	axis_test_reply_clear(&reply);
}

static void
test_ico_has_three_sizes(Fixture *fx, gconstpointer data)
{
	AxisTestReply reply = fetch(fx, "/favicon.ico", NULL);
	const guint8 *b = (const guint8 *)reply.body;
	static const guint sizes[] = { 16, 32, 48 };
	guint i;

	g_assert_cmpuint(reply.status, ==, 200);
	/* Reserved 0, type 1 (icon), three images. */
	g_assert_cmpuint(b[0] | (b[1] << 8), ==, 0);
	g_assert_cmpuint(b[2] | (b[3] << 8), ==, 1);
	g_assert_cmpuint(b[4] | (b[5] << 8), ==, 3);
	for (i = 0; i < 3; i++) {
		const guint8 *entry = b + 6 + i * 16;

		g_assert_cmpuint(entry[0], ==, sizes[i]);
		g_assert_cmpuint(entry[1], ==, sizes[i]);
	}

	axis_test_reply_clear(&reply);
}

/* Every icon link of a page: it is served with the right type and, for a
 * bitmap, the size its tag states. */
static void
test_declared_icons_exist(Fixture *fx, gconstpointer data)
{
	AxisTestReply page = fetch(fx, "/", fx->cookie);
	AxisTestReply login = fetch(fx, "/login", NULL);
	AxisTestReply *pages[] = { &page, &login };
	static const gchar *const rels[] = { "icon", "apple-touch-icon", "mask-icon", "manifest", NULL };
	guint seen_rels[4] = { 0, 0, 0, 0 };
	guint p;
	guint i;

	for (p = 0; p < G_N_ELEMENTS(pages); p++) {
		for (i = 0;; i++) {
			g_autofree gchar *tag = nth_tag(pages[p]->body, "<link", i);
			g_autofree gchar *rel = NULL;
			g_autofree gchar *href = NULL;
			g_autofree gchar *sizes = NULL;
			g_autofree gchar *type = NULL;
			AxisTestReply icon;
			guint width = 0;
			guint height = 0;
			guint r;

			if (tag == NULL)
				break;
			rel = tag_attr(tag, "rel");
			if (rel == NULL || !in_list(rels, rel))
				continue;
			href = tag_attr(tag, "href");
			sizes = tag_attr(tag, "sizes");
			type = tag_attr(tag, "type");
			g_assert_nonnull(href);
			g_assert_true(g_str_has_prefix(href, AXIS_WEB_STATIC_PREFIX));

			icon = fetch(fx, href, NULL);
			g_assert_cmpuint(icon.status, ==, 200);
			if (g_str_has_suffix(href, ".png")) {
				g_assert_true(parse_sizes(sizes, &width, &height));
				assert_png(&icon, width, height);
				g_assert_cmpstr(type != NULL ? type : "image/png", ==, "image/png");
			} else if (g_str_has_suffix(href, ".svg")) {
				g_assert_cmpstr(axis_test_reply_header(&icon, "Content-Type"), ==, "image/svg+xml");
				assert_contains(icon.body, "<svg");
			} else if (g_str_has_suffix(href, ".webmanifest")) {
				g_assert_cmpstr(axis_test_reply_header(&icon, "Content-Type"), ==,
					"application/manifest+json");
			} else {
				g_error("a link to %s that is not an icon type this test knows", href);
			}
			if (type != NULL)
				g_assert_cmpstr(axis_test_reply_header(&icon, "Content-Type"), ==, type);
			axis_test_reply_clear(&icon);

			for (r = 0; rels[r] != NULL; r++)
				seen_rels[r] += strcmp(rels[r], rel) == 0;
		}
	}

	/* Each page declares a vector and a bitmap tab icon (two "icon"
	 * links), the touch icon, the pinned-tab icon and the manifest. */
	g_assert_cmpuint(seen_rels[0], ==, 4);
	g_assert_cmpuint(seen_rels[1], ==, 2);
	g_assert_cmpuint(seen_rels[2], ==, 2);
	g_assert_cmpuint(seen_rels[3], ==, 2);

	{
		g_autofree gchar *title = meta_attr(page.body, "apple-mobile-web-app-title", 0, "content");
		g_autofree gchar *status = meta_attr(page.body, "apple-mobile-web-app-status-bar-style", 0,
			"content");

		g_assert_cmpstr(title, ==, "Axis");
		g_assert_nonnull(status);
	}
	{
		g_autofree gchar *mask = find_tag(page.body, "<link", "rel", "mask-icon");
		g_autofree gchar *colour = tag_attr(mask, "color");

		g_assert_true(is_hex6(colour));
	}
	{
		g_autofree gchar *touch = find_tag(page.body, "<link", "rel", "apple-touch-icon");
		g_autofree gchar *sizes = tag_attr(touch, "sizes");

		g_assert_cmpstr(sizes, ==, "180x180");
	}

	axis_test_reply_clear(&page);
	axis_test_reply_clear(&login);
}

static void
test_manifest(Fixture *fx, gconstpointer data)
{
	AxisTestReply page = fetch(fx, "/", fx->cookie);
	g_autofree gchar *tag = find_tag(page.body, "<link", "rel", "manifest");
	g_autofree gchar *href = tag_attr(tag, "href");
	g_autofree gchar *directory = g_path_get_dirname(href);
	g_autofree gchar *css = asset_body(fx, "themes.css");
	g_autoptr(GPtrArray) blocks = parse_themes(css);
	const ThemeBlock *midnight = block_for(blocks, "midnight", FALSE);
	AxisTestReply manifest = fetch(fx, href, NULL);
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(GError) error = NULL;
	JsonObject *root;
	JsonArray *icons;
	gboolean has_192 = FALSE;
	gboolean has_512 = FALSE;
	gboolean has_maskable = FALSE;
	guint i;

	g_assert_cmpuint(manifest.status, ==, 200);
	g_assert_cmpstr(axis_test_reply_header(&manifest, "Content-Type"), ==, "application/manifest+json");
	g_assert_true(json_parser_load_from_data(parser, manifest.body, -1, &error));
	g_assert_no_error(error);
	g_assert_true(JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)));
	root = json_node_get_object(json_parser_get_root(parser));

	g_assert_cmpstr(json_object_get_string_member(root, "name"), ==, "Axis");
	g_assert_cmpstr(json_object_get_string_member(root, "short_name"), ==, "Axis");
	g_assert_cmpstr(json_object_get_string_member(root, "start_url"), ==, "/");
	g_assert_cmpstr(json_object_get_string_member(root, "display"), ==, "standalone");
	/* A manifest cannot follow a user's choice: it has the default theme's. */
	g_assert_cmpstr(json_object_get_string_member(root, "theme_color"), ==,
		axis_web_theme_find(AXIS_WEB_THEME_FALLBACK)->theme_color);
	g_assert_cmpstr(json_object_get_string_member(root, "background_color"), ==, var_of(midnight, "bg"));

	icons = json_object_get_array_member(root, "icons");
	g_assert_nonnull(icons);
	for (i = 0; i < json_array_get_length(icons); i++) {
		JsonObject *icon = json_array_get_object_element(icons, i);
		const gchar *src = json_object_get_string_member(icon, "src");
		const gchar *sizes = json_object_get_string_member(icon, "sizes");
		const gchar *purpose = json_object_has_member(icon, "purpose") ?
			json_object_get_string_member(icon, "purpose") : "any";
		/* An icon's address is relative to the manifest's. */
		g_autofree gchar *path = g_strconcat(directory, "/", src, NULL);
		AxisTestReply reply = fetch(fx, path, NULL);
		guint width = 0;
		guint height = 0;

		g_assert_true(parse_sizes(sizes, &width, &height));
		assert_png(&reply, width, height);
		g_assert_cmpstr(json_object_get_string_member(icon, "type"), ==, "image/png");
		has_192 |= width == 192 && strcmp(purpose, "any") == 0;
		has_512 |= width == 512 && strcmp(purpose, "any") == 0;
		has_maskable |= width == 512 && strcmp(purpose, "maskable") == 0;
		axis_test_reply_clear(&reply);
	}
	g_assert_true(has_192);
	g_assert_true(has_512);
	g_assert_true(has_maskable);

	axis_test_reply_clear(&page);
	axis_test_reply_clear(&manifest);
}

static void
test_logo_and_tab_icon_types(Fixture *fx, gconstpointer data)
{
	static const struct
	{
		const gchar *asset;
		const gchar *type;
	} types[] = {
		{ "icons/favicon.svg", "image/svg+xml" },
		{ "icons/logo-mark.svg", "image/svg+xml" },
		{ "icons/safari-pinned-tab.svg", "image/svg+xml" },
		{ "icons/favicon.ico", "image/x-icon" },
		{ "icons/favicon-32.png", "image/png" },
		{ "icons/icon-192.png", "image/png" },
		{ "icons/icon-512.png", "image/png" },
		{ "icons/icon-maskable-512.png", "image/png" },
		{ "icons/manifest.webmanifest", "application/manifest+json" },
		{ NULL, NULL }
	};
	guint i;

	for (i = 0; types[i].asset != NULL; i++) {
		g_autofree gchar *url = axis_web_asset_url(types[i].asset);
		AxisTestReply reply = fetch(fx, url, NULL);

		g_assert_cmpuint(reply.status, ==, 200);
		g_assert_cmpstr(axis_test_reply_header(&reply, "Content-Type"), ==, types[i].type);
		axis_test_reply_clear(&reply);
	}

	/* The generator and the notes are not served. */
	{
		g_autofree gchar *script = axis_web_asset_url("icons/generate.sh");
		g_autofree gchar *readme = axis_web_asset_url("icons/README.md");
		AxisTestReply a = fetch(fx, script, NULL);
		AxisTestReply b = fetch(fx, readme, NULL);

		g_assert_cmpuint(a.status, ==, 404);
		g_assert_cmpuint(b.status, ==, 404);
		axis_test_reply_clear(&a);
		axis_test_reply_clear(&b);
	}
}

/* ── Nothing else became public ─────────────────────────────────── */

static void
test_only_three_paths_became_public(Fixture *fx, gconstpointer data)
{
	static const gchar *const public_paths[] = {
		"/healthz", "/livez", "/readyz", "/metrics", "/login", "/setup", "/favicon.ico",
		"/apple-touch-icon.png", "/apple-touch-icon-precomposed.png", "/static/0123456789ab/axis.css", NULL
	};
	static const gchar *const private_paths[] = {
		"/", "/account", "/admin/users", "/favicon.icox", "/favicon.ico/", "/favicon.png",
		"/FAVICON.ICO", "/apple-touch-icon-120x120.png", "/apple-touch-icon-180x180.png",
		"/apple-touch-icon-precomposed.png/", "/apple-touch-icon", "/apple-touch-icon.ico",
		"/manifest.webmanifest", "/site.webmanifest", "/robots.txt", "/icons/favicon.ico",
		"/safari-pinned-tab.svg", "/static", "/staticx/", "//favicon.ico", "/.well-known/favicon.ico",
		"/browserconfig.xml", "/livez/", "/readyz/x", "/metrics/", "/metricsx", "/LIVEZ", NULL
	};
	guint i;

	for (i = 0; public_paths[i] != NULL; i++)
		g_assert_true(axis_web_auth_is_public_path(public_paths[i]));
	for (i = 0; private_paths[i] != NULL; i++) {
		AxisTestReply reply;

		g_assert_false(axis_web_auth_is_public_path(private_paths[i]));
		/* A client without a session is sent to log in, as before. */
		reply = fetch(fx, private_paths[i], NULL);
		if (reply.status != 303 && reply.status != 404 && reply.status != 400)
			g_error("%s without a session answered %u", private_paths[i], reply.status);
		if (reply.status == 303)
			g_assert_true(g_str_has_prefix(axis_test_reply_header(&reply, "Location"), "/login"));
		else
			g_assert_null(strstr(reply.body, "<svg"));
		axis_test_reply_clear(&reply);
	}

	/* Routes: exactly the three paths may be registered as public. */
	g_assert_true(axis_web_auth_is_public_pattern("/favicon.ico"));
	g_assert_true(axis_web_auth_is_public_pattern("/apple-touch-icon.png"));
	g_assert_true(axis_web_auth_is_public_pattern("/apple-touch-icon-precomposed.png"));
	g_assert_false(axis_web_auth_is_public_pattern("/favicon.*"));
	g_assert_false(axis_web_auth_is_public_pattern("/apple-touch-icon*"));
	g_assert_false(axis_web_auth_is_public_pattern("/apple-touch-icon-:size.png"));
	g_assert_false(axis_web_auth_is_public_pattern("/*"));
	g_assert_false(axis_web_auth_is_public_pattern("/manifest.webmanifest"));
}

/* ── The picker ─────────────────────────────────────────────────── */

/* The values of the radio inputs called "theme", and which is checked. */
static guint
radios(
	const gchar *html,
	gchar      **checked
){
	guint count = 0;
	guint i;

	*checked = NULL;
	for (i = 0;; i++) {
		g_autofree gchar *tag = nth_tag(html, "<input", i);
		g_autofree gchar *name = NULL;

		if (tag == NULL)
			break;
		name = tag_attr(tag, "name");
		if (g_strcmp0(name, "theme") != 0)
			continue;
		count++;
		if (strstr(tag, " checked") != NULL) {
			g_assert_null(*checked);
			*checked = tag_attr(tag, "value");
		}
	}

	return count;
}

static void
test_picker_lists_every_theme(Fixture *fx, gconstpointer data)
{
	AxisTestReply page;
	g_autofree gchar *checked = NULL;
	guint i;

	axis_web_auth_set_test_theme(fx->auth, "tester", "dusk");
	page = fetch(fx, "/account", fx->cookie);
	g_assert_cmpuint(page.status, ==, 200);

	/* One radio per theme and one for the default, the user's own checked. */
	g_assert_cmpuint(radios(page.body, &checked), ==, axis_web_theme_count() + 1);
	g_assert_cmpstr(checked, ==, "dusk");
	assert_contains(page.body, "action=\"/account/theme\"");
	assert_contains(page.body, "name=\"_csrf\"");

	/* Each preview is built from elements carrying that theme's own
	 * attribute (so it sits inside this page, which has another). */
	for (i = 0; i < axis_web_theme_count(); i++) {
		g_autofree gchar *needle = g_strdup_printf("class=\"swatch\" data-theme=\"%s\"",
			axis_web_theme_at(i)->id);
		g_autofree gchar *alt = g_strdup_printf("data-theme=\"%s\" class=\"swatch\"",
			axis_web_theme_at(i)->id);

		if (strstr(page.body, needle) == NULL && strstr(page.body, alt) == NULL)
			g_error("no preview of %s on the account page", axis_web_theme_at(i)->id);
		assert_contains(page.body, axis_web_theme_at(i)->name);
	}
	/* The page itself, the nine themes and the default's own preview. */
	g_assert_cmpuint(count_of(page.body, "data-theme=\""), ==, 1 + axis_web_theme_count() + 1);
	assert_no_inline_code(page.body);
	axis_test_reply_clear(&page);

	/* Nothing chosen: "use the default" is the checked one. */
	axis_web_auth_set_test_theme(fx->auth, "tester", NULL);
	page = fetch(fx, "/account", fx->cookie);
	g_free(checked);
	g_assert_cmpuint(radios(page.body, &checked), ==, axis_web_theme_count() + 1);
	g_assert_cmpstr(checked, ==, "default");
	axis_test_reply_clear(&page);

	/* A theme this version no longer has reads as none. */
	axis_web_auth_set_test_theme(fx->auth, "tester", "retired-theme");
	page = fetch(fx, "/account", fx->cookie);
	g_free(checked);
	radios(page.body, &checked);
	g_assert_cmpstr(checked, ==, "default");
	axis_test_reply_clear(&page);
}

/* Spec: Previews show the difference.  Each preview is a small page in its
 * theme: the page colour, a card, the accent and the mark in the theme's
 * own colours. */
static void
test_picker_previews(Fixture *fx, gconstpointer data)
{
	AxisTestReply page = fetch(fx, "/account", fx->cookie);
	guint i;

	g_assert_cmpuint(page.status, ==, 200);
	for (i = 0; i < axis_web_theme_count(); i++) {
		const AxisWebTheme *theme = axis_web_theme_at(i);
		g_autofree gchar *needle = g_strdup_printf("value=\"%s\"", theme->id);
		g_autofree gchar *colours = g_strdup_printf("data-theme=\"%s\"", theme->id);
		g_autofree gchar *mask = g_strdup_printf("id=\"axis-hub-swatch-%s\"", theme->id);
		const gchar *start = strstr(page.body, needle);
		const gchar *end;
		g_autofree gchar *preview = NULL;

		/* The option's radio, then its preview, up to the theme's name. */
		g_assert_nonnull(start);
		end = strstr(start, "class=\"theme-name\"");
		g_assert_nonnull(end);
		preview = g_strndup(start, end - start);
		assert_contains(preview, colours);
		assert_contains(preview, "class=\"swatch-bar\"");
		assert_contains(preview, "class=\"swatch-card\"");
		assert_contains(preview, "class=\"swatch-chip\"");
		assert_contains(preview, "class=\"logo-step-1\"");
		assert_contains(preview, "class=\"logo-line\"");
		assert_contains(preview, mask);
	}
	/* The default's own preview, and every mask id once. */
	assert_contains(page.body, "id=\"axis-hub-swatch-default\"");
	g_assert_cmpuint(count_of(page.body, "id=\"axis-hub-swatch-oxblood\""), ==, 1);
	assert_no_inline_code(page.body);
	axis_test_reply_clear(&page);
}

static void
test_picker_refuses_forged_and_unknown(Fixture *fx, gconstpointer data)
{
	AxisTestReply page;
	AxisTestReply reply;
	g_autofree gchar *token = NULL;
	g_autofree gchar *form = NULL;
	g_autofree gchar *id = NULL;

	axis_web_auth_set_test_theme(fx->auth, "tester", "dusk");

	/* Without a CSRF token: refused, and nothing changed. */
	reply = fetch_with(fx, "POST", "/account/theme", fx->cookie, "theme=paper");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);

	/* With the token of another session: refused too. */
	page = fetch(fx, "/account", fx->other_cookie);
	token = axis_test_web_csrf_token(page.body);
	g_assert_nonnull(token);
	axis_test_reply_clear(&page);
	form = g_strdup_printf("_csrf=%s&theme=paper", token);
	reply = fetch_with(fx, "POST", "/account/theme", fx->cookie, form);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);

	page = fetch(fx, "/", fx->cookie);
	id = html_theme(page.body);
	g_assert_cmpstr(id, ==, "dusk");
	axis_test_reply_clear(&page);

	/* With the right token, a theme that does not exist is not stored. */
	page = fetch(fx, "/account", fx->cookie);
	g_free(token);
	token = axis_test_web_csrf_token(page.body);
	g_assert_nonnull(token);
	axis_test_reply_clear(&page);
	g_free(form);
	form = g_strdup_printf("_csrf=%s&theme=neon", token);
	reply = fetch_with(fx, "POST", "/account/theme", fx->cookie, form);
	g_assert_cmpuint(reply.status, ==, 400);
	assert_contains(reply.body, "Choose one of the themes listed.");
	assert_lacks(reply.body, "neon");
	axis_test_reply_clear(&reply);

	g_free(form);
	form = g_strdup_printf("_csrf=%s", token);
	reply = fetch_with(fx, "POST", "/account/theme", fx->cookie, form);
	g_assert_cmpuint(reply.status, ==, 400);
	axis_test_reply_clear(&reply);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/web-themes/stylesheet/detector", test_detector_catches_colours);
	g_test_add_func("/web-themes/contrast/arithmetic", test_contrast_code_is_right);
	g_test_add_func("/web-themes/distance/arithmetic", test_delta_e_is_right);

#define ADD(name, fn) \
	g_test_add("/web-themes/" name, Fixture, NULL, fixture_setup, fn, fixture_teardown)

	ADD("stylesheet/no-colour-outside-themes", test_stylesheet_has_no_colour);
	ADD("stylesheet/themes-are-complete", test_themes_are_complete);
	ADD("contrast/every-theme", test_theme_contrast);
	ADD("stylesheet/logo-colours", test_logo_colours);
	ADD("stylesheet/oxblood-brand-and-action", test_oxblood_brand_and_action);
	ADD("stylesheet/auto-follows-the-device", test_auto_follows_the_device);
	ADD("stylesheet/linen-brand-and-action", test_linen_brand_and_action);
	ADD("stylesheet/theme-color-matches", test_theme_color_matches_stylesheet);
	ADD("distance/backgrounds", test_backgrounds_are_distinct);
	ADD("stylesheet/brand-leads", test_brand_leads);
	ADD("stylesheet/one-control-height", test_one_control_height);
	ADD("stylesheet/column-widths", test_column_widths_agree);
	ADD("stylesheet/every-film-mark-is-styled", test_every_film_mark_is_styled);
	ADD("stylesheet/every-episode-mark-is-styled", test_every_episode_mark_is_styled);

	ADD("page/default-is-dark", test_default_is_dark_for_everyone);
	ADD("page/every-theme-renders", test_every_theme_renders);
	ADD("page/unknown-theme-falls-back", test_unknown_theme_falls_back);
	ADD("page/users-are-separate", test_users_themes_are_separate);
	ADD("page/unsigned-visitor-midnight", test_unsigned_visitor_gets_midnight);
	ADD("page/unknown-or-empty-theme-midnight", test_unknown_or_empty_stored_theme_gets_midnight);
	ADD("stylesheet/no-attribute-midnight", test_stylesheet_without_attribute_is_midnight);
	ADD("page/default-applies-to-unchosen",test_default_theme_applies_to_those_without_a_choice);
	ADD("page/mark-is-inline", test_mark_is_drawn_inline);

	ADD("icons/well-known-paths", test_well_known_icons);
	ADD("icons/touch-icon-is-opaque", test_touch_icon_is_opaque);
	ADD("icons/ico-sizes", test_ico_has_three_sizes);
	ADD("icons/declared-icons-exist", test_declared_icons_exist);
	ADD("icons/manifest", test_manifest);
	ADD("icons/types", test_logo_and_tab_icon_types);
	ADD("icons/nothing-else-public", test_only_three_paths_became_public);

	ADD("picker/lists-every-theme", test_picker_lists_every_theme);
	ADD("picker/previews", test_picker_previews);
	ADD("picker/forged-and-unknown", test_picker_refuses_forged_and_unknown);

	return g_test_run();
}
