/*
 * axis-def.h - an indexer definition (the "Cardigann" YAML format of
 * Prowlarr and Jackett), read and judged
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A definition describes one site: its addresses, what it can be searched
 * for and in which categories, how to build its search request, how to find
 * each release in its answer, and how each field of a release is read.  It
 * is the administrator's data, written by people Axis does not know, and is
 * read here as hostile text (indexer/def/axis-def-yaml.h).
 *
 * axis_def_load() reads one and judges it:
 *
 *  - Not a definition (%NULL, with the reason): YAML that is refused, no
 *    map, no id, name, links or search, an id that is not 1 to 64 of
 *    letters, digits, '.', '-' and '_', a link that is not an http or https
 *    address.  Such a file is not kept.
 *  - A definition that is not usable: one that needs something the engine
 *    does not have (a login, a POST search, a selector or filter it lacks,
 *    a key typed in...), or that asks for what Axis never does (a search on
 *    an address that is not one of its site's), or that has nothing Axis
 *    searches for (no movie, TV or general category).  Its needs are listed
 *    in plain words (#AxisDefNeed), and it is kept but never used: a
 *    definition either works whole or not at all.
 *  - A usable one: every selector, template and regular expression in it
 *    reads, every filter is known, and nothing it needs is missing.
 *
 * What the engine supports is a first cut: what a survey of the public
 * definitions found most of them use (openspec/specs/indexers/spec.md).  Settings take their defaults (an indexer's
 * own values are not editable in this stage).
 *
 * Two keys are Axis's own, written in the definitions Axis ships
 * (add-builtin-torrent-indexers) and in an administrator's override of one:
 * "version" at the top, the file's revision (a whole number from 1; one
 * without it is version 0), and "expect" in search.rows, what every answer of
 * the site has whether it lists results or not (a selector, or a JSON path),
 * by which the engine tells an empty answer from a page that is not the one
 * the definition reads (indexer/def/axis-def-engine.h).
 *
 * GLib only, no I/O: read by tests/fuzz/fuzz-def-yaml.c.
 */

#ifndef AXIS_DEF_H
#define AXIS_DEF_H

#include <glib.h>

G_BEGIN_DECLS

/* The largest definition accepted, in bytes (the largest public one is
 * 64 kB). */
#define AXIS_DEF_MAX_BYTES     (256 * 1024)

/* The highest version a definition may state. */
#define AXIS_DEF_MAX_VERSION   1000000

/* At most this many of each list are read: links, categories, settings,
 * search paths, inputs, fields, a field's filters, download selectors. */
#define AXIS_DEF_MAX_LINKS      64
#define AXIS_DEF_MAX_CATEGORIES 512
#define AXIS_DEF_MAX_SETTINGS   64
#define AXIS_DEF_MAX_PATHS      16
#define AXIS_DEF_MAX_INPUTS     64
#define AXIS_DEF_MAX_FIELDS     128
#define AXIS_DEF_MAX_FILTERS    64

#define AXIS_DEF_MODE_SEARCH  1
#define AXIS_DEF_MODE_TV      2
#define AXIS_DEF_MODE_MOVIE   4

#define AXIS_DEF_PARAM_Q       1
#define AXIS_DEF_PARAM_SEASON  2
#define AXIS_DEF_PARAM_EP      4
#define AXIS_DEF_PARAM_IMDB    8
#define AXIS_DEF_PARAM_TVDB    16
#define AXIS_DEF_PARAM_TMDB    32

/* A filter: its name and its arguments as text. */
typedef struct
{
	gchar  *name;
	gchar **args;	/* %NULL-ended */
} AxisDefFilter;

/**
 * AxisDefBlock:
 * @name: a field's name ("title", "_id", "title_default"); "" elsewhere
 * @selector: (nullable): a selector (a JSON path for a JSON answer)
 * @text: (nullable): a template whose text is the value
 * @attribute: (nullable): the attribute read instead of the text
 * @optional: a value not found is the default (or empty), not a row lost
 * @default_text: (nullable): that default, a template
 * @remove: (nullable): a selector of what is left out of the text
 * @case_keys, @case_values: (element-type utf8): the value is the first
 *   case's whose key (a selector; "*" for any) matches
 * @filters: (element-type AxisDefFilter): applied in order
 *
 * A selector block: a field, a download selector, an error's message.
 */
typedef struct
{
	gchar     *name;
	gchar     *selector;
	gchar     *text;
	gchar     *attribute;
	gboolean   optional;
	gchar     *default_text;
	gchar     *remove;
	GPtrArray *case_keys;
	GPtrArray *case_values;
	GPtrArray *filters;
} AxisDefBlock;

/* A name and a value (an input, a header). */
typedef struct
{
	gchar *name;
	gchar *value;
} AxisDefPair;

typedef enum
{
	AXIS_DEF_HTML,
	AXIS_DEF_JSON,
	AXIS_DEF_XML
} AxisDefResponse;

/**
 * AxisDefPath:
 * @path: a template: a path relative to the site, absolute, or an address
 * @response: how its answer is read
 * @inputs: (element-type AxisDefPair): its own inputs
 * @inherit_inputs: the search's inputs are sent too (the default)
 * @categories: (element-type utf8) (nullable): the site's categories this
 *   path is asked for (any, when %NULL)
 * @query_separator: between the inputs ("&")
 */
typedef struct
{
	gchar           *path;
	AxisDefResponse  response;
	GPtrArray       *inputs;
	gboolean         inherit_inputs;
	GPtrArray       *categories;
	gchar           *query_separator;
} AxisDefPath;

/**
 * AxisDefCategory:
 * @site_id: the site's own identifier of the category
 * @newznab: its Newznab number (2040 for "Movies/HD"), 0 when unknown
 * @desc: (nullable): the site's words for it
 */
typedef struct
{
	gchar *site_id;
	gint   newznab;
	gchar *desc;
} AxisDefCategory;

/**
 * AxisDefSetting:
 * @name: as the templates name it (.Config.@name)
 * @type: "text", "checkbox", "select", "password", "info"...
 * @label: (nullable)
 * @value: its default as the templates see it: a checkbox's is "True" or
 *   "", a select's its option's key
 */
typedef struct
{
	gchar *name;
	gchar *type;
	gchar *label;
	gchar *value;
} AxisDefSetting;

/* An error block: when @selector matches the answer, the search fails with
 * @message's text. */
typedef struct
{
	gchar        *selector;
	AxisDefBlock *message;
} AxisDefError;

/**
 * AxisDefNeed:
 * @what: in plain words ("a login", "searches sent as a form (POST)")
 * @never: not a feature to come: what Axis does not do ("searches an
 *   address that is not one of its site's")
 */
typedef struct
{
	gchar    *what;
	gboolean  never;
} AxisDefNeed;

/**
 * AxisDef:
 *
 * A definition as read.  The lists are of the types named; each is
 * present (perhaps empty).
 */
typedef struct
{
	gchar           *id;
	gchar           *name;
	gchar           *description;
	gchar           *language;
	gchar           *site_type;
	gchar           *encoding;
	gint             version;	/* its revision, 0 when it states none */
	GPtrArray       *links;	/* utf8: the site's addresses, first the preferred */
	GPtrArray       *legacy_links;	/* utf8 */
	gdouble          request_delay;	/* seconds between its requests, 0 for none */
	GPtrArray       *categories;	/* AxisDefCategory */
	guint            modes;
	guint            tv_params;
	guint            movie_params;
	GPtrArray       *settings;	/* AxisDefSetting */
	GPtrArray       *paths;	/* AxisDefPath */
	GPtrArray       *inputs;	/* AxisDefPair */
	gboolean         allow_empty_inputs;
	GPtrArray       *headers;	/* AxisDefPair */
	GPtrArray       *keywords_filters;	/* AxisDefFilter */
	GPtrArray       *errors;	/* AxisDefError */
	gchar           *rows_selector;
	gchar           *rows_expect;	/* nullable: what every answer has */
	gint             rows_after;
	gboolean         rows_andmatch;
	GPtrArray       *fields;	/* AxisDefBlock, in order */
	GPtrArray       *download_selectors;	/* AxisDefBlock */
	AxisDefBlock    *download_hash;	/* nullable */
	AxisDefBlock    *download_title;	/* nullable */
	gboolean         covers_movies;
	gboolean         covers_tv;
	gboolean         general;	/* every category is "Other": searched by words */
	GPtrArray       *needs;	/* AxisDefNeed */
	gboolean         challenge_hint;	/* an info_flaresolverr setting: the site is known to challenge */
} AxisDef;

/**
 * axis_def_load:
 * @data: the file
 * @length: its length in bytes
 * @problem: (out) (optional) (transfer full): why it is not a definition
 *
 * Returns: (transfer full) (nullable): the definition, judged
 */
AxisDef *
axis_def_load(
	const gchar  *data,
	gsize         length,
	gchar       **problem
);

void
axis_def_free(AxisDef *def);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisDef, axis_def_free)

/* Whether nothing is missing: it can be used. */
gboolean
axis_def_usable(const AxisDef *def);

/* Why it cannot be used, in a sentence ("Needs a login and searches sent as
 * a form (POST)."), or %NULL for a usable one. */
gchar *
axis_def_problem(const AxisDef *def);

/* The Newznab number of a category's name ("Movies/HD" is 2040), 0 for a
 * name that is not one. */
gint
axis_def_newznab_category(const gchar *name);

/* The setting @name, or %NULL. */
const AxisDefSetting *
axis_def_setting(
	const AxisDef *def,
	const gchar   *name
);

/* The field @name, or %NULL. */
const AxisDefBlock *
axis_def_field(
	const AxisDef *def,
	const gchar   *name
);

/* Whether @link is one of the definition's addresses (links or
 * legacylinks), compared by scheme, host and port. */
gboolean
axis_def_has_link(
	const AxisDef *def,
	const gchar   *link
);

G_END_DECLS

#endif /* AXIS_DEF_H */
