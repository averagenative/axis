/*
 * axis-web-analysis-chart.c - charts as inline SVG, built with the element
 * builder
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The content security policy forbids inline styles (a style attribute or
 * a style element, inside SVG too), so a chart's geometry is in attributes
 * (x, y, width, points) and its colours are classes in the stylesheet
 * (data/web/axis.css).  htmx-glib has no SVG element types, but its
 * generic node (htmx_node_new) takes any tag name; "rect", "text" and
 * the rest render as ordinary elements with closing tags, which HTML's
 * parser accepts inside an <svg>.  Numbers in attributes are integers
 * written by hand, so that the locale cannot put a comma in a coordinate.
 * Everything that came from a name is text content (escaped), never an
 * attribute; the only attribute values built from outside are the links,
 * which the pages make of safe characters.
 */

#include "web/axis-web-analysis-private.h"

#include <string.h>

#define CHART_WIDTH     760
#define ROW_HEIGHT      26
#define LABEL_CHARS     30
#define BAR_X           240
#define BAR_MAX         320
#define VALUE_X         572
#define MAX_ROWS        30

#define PLOT_LEFT       70
#define PLOT_RIGHT      740
#define PLOT_TOP        24
#define PLOT_BOTTOM     180
#define GROWTH_HEIGHT   220
#define MAX_POINTS      120

static HtmxNode *
svg_new(const gchar *tag)
{
	return htmx_node_new(tag);
}

static void
set_attr(
	HtmxNode    *node,
	const gchar *name,
	const gchar *value
){
	htmx_element_set_attribute(HTMX_ELEMENT(node), name, value);
}

static void
set_int(
	HtmxNode    *node,
	const gchar *name,
	gint         value
){
	g_autofree gchar *text = g_strdup_printf("%d", value);

	set_attr(node, name, text);
}

static void
set_class(
	HtmxNode    *node,
	const gchar *name
){
	htmx_element_add_class(HTMX_ELEMENT(node), name);
}

static HtmxNode *
svg_text(
	const gchar *css,
	gint         x,
	gint         y,
	const gchar *text
){
	HtmxNode *node = svg_new("text");

	set_class(node, css);
	set_int(node, "x", x);
	set_int(node, "y", y);
	htmx_node_set_text_content(node, text);
	return node;
}

static HtmxNode *
svg_title(const gchar *text)
{
	HtmxNode *node = svg_new("title");

	htmx_node_set_text_content(node, text);
	return node;
}

/* @text cut to @chars characters, with an ellipsis when it was longer. */
static gchar *
shorten(
	const gchar *text,
	glong        chars
){
	if (text == NULL)
		return g_strdup("");
	if (!g_utf8_validate(text, -1, NULL))
		return g_strdup("");
	if (g_utf8_strlen(text, -1) <= chars)
		return g_strdup(text);
	{
		const gchar *end = g_utf8_offset_to_pointer(text, chars - 1);
		g_autofree gchar *head = g_strndup(text, (gsize)(end - text));

		return g_strconcat(head, "\xe2\x80\xa6", NULL);
	}
}

/* ── Bars ───────────────────────────────────────────────────────── */

AxisChartBar *
axis_chart_bar_new(
	const gchar *label,
	gint64       value,
	const gchar *value_text,
	const gchar *href,
	gboolean     muted
){
	AxisChartBar *bar = g_new0(AxisChartBar, 1);

	bar->label = g_strdup(label != NULL ? label : "");
	bar->value = value;
	bar->value_text = g_strdup(value_text != NULL ? value_text : "");
	bar->href = g_strdup(href);
	bar->muted = muted;
	return bar;
}

void
axis_chart_bar_free(AxisChartBar *bar)
{
	if (bar == NULL)
		return;

	g_free(bar->label);
	g_free(bar->value_text);
	g_free(bar->href);
	g_free(bar);
}

HtmxNode *
axis_web_analysis_bar_chart(
	const gchar *title,
	GPtrArray   *bars
){
	HtmxNode *svg = svg_new("svg");
	g_autofree gchar *box = NULL;
	gint64 largest = 0;
	guint rows = MIN(bars->len, (guint)MAX_ROWS);
	guint i;

	for (i = 0; i < rows; i++) {
		const AxisChartBar *bar = g_ptr_array_index(bars, i);

		largest = MAX(largest, bar->value);
	}

	box = g_strdup_printf("0 0 %d %d", CHART_WIDTH, (gint)(rows * ROW_HEIGHT + 2));
	set_class(svg, "chart");
	set_attr(svg, "viewBox", box);
	set_attr(svg, "role", "img");
	set_attr(svg, "aria-label", title);
	BROWSER_ADD(svg, svg_title(title));

	for (i = 0; i < rows; i++) {
		const AxisChartBar *bar = g_ptr_array_index(bars, i);
		HtmxNode *row = svg_new(bar->href != NULL ? "a" : "g");
		HtmxNode *rect = svg_new("rect");
		g_autofree gchar *label = shorten(bar->label, LABEL_CHARS);
		g_autofree gchar *hover = g_strdup_printf("%s: %s", bar->label, bar->value_text);
		gint top = (gint)(i * ROW_HEIGHT);
		gint width = 0;

		if (bar->value > 0 && largest > 0) {
			/* A fraction of the longest bar (a double, so that byte counts
			 * cannot overflow), rounded to a whole number of units. */
			width = (gint)((gdouble)bar->value * BAR_MAX / (gdouble)largest + 0.5);
			width = CLAMP(width, 1, BAR_MAX);
		}

		set_class(row, "chart-row");
		if (bar->href != NULL)
			set_attr(row, "href", bar->href);
		BROWSER_ADD(row, svg_title(hover));
		BROWSER_ADD(row, svg_text("chart-label", 0, top + 18, label));

		set_class(rect, bar->muted ? "chart-bar chart-bar-muted" : "chart-bar");
		set_int(rect, "x", BAR_X);
		set_int(rect, "y", top + 4);
		set_int(rect, "width", width);
		set_int(rect, "height", ROW_HEIGHT - 8);
		BROWSER_ADD(row, rect);
		BROWSER_ADD(row, svg_text("chart-value", VALUE_X, top + 18, bar->value_text));
		BROWSER_ADD(svg, row);
	}
	return svg;
}

/* ── Growth ─────────────────────────────────────────────────────── */

static gchar *
day_of(gint64 us)
{
	GDateTime *time = g_date_time_new_from_unix_utc(us / G_USEC_PER_SEC);
	gchar *text;

	if (time == NULL)
		return g_strdup("");
	text = g_date_time_format(time, "%Y-%m-%d");
	g_date_time_unref(time);
	return text;
}

HtmxNode *
axis_web_analysis_growth_chart(
	const gchar *title,
	GPtrArray   *points
){
	HtmxNode *svg = svg_new("svg");
	HtmxNode *axis = svg_new("line");
	HtmxNode *line = svg_new("polyline");
	g_autofree gchar *box = g_strdup_printf("0 0 %d %d", CHART_WIDTH, GROWTH_HEIGHT);
	g_autofree gchar *top_label = NULL;
	g_autofree gchar *first_day = NULL;
	g_autofree gchar *last_day = NULL;
	GString *coordinates = g_string_new(NULL);
	gint64 largest = 0;
	gint64 first_us = 0;
	gint64 last_us = 0;
	guint step;
	guint i;

	set_class(svg, "chart chart-growth");
	set_attr(svg, "viewBox", box);
	set_attr(svg, "role", "img");
	set_attr(svg, "aria-label", title);
	BROWSER_ADD(svg, svg_title(title));

	if (points->len == 0) {
		g_string_free(coordinates, TRUE);
		BROWSER_ADD(svg, axis);
		BROWSER_ADD(svg, line);
		return svg;
	}
	for (i = 0; i < points->len; i++) {
		const AxisAnalysisPoint *point = g_ptr_array_index(points, i);

		largest = MAX(largest, point->bytes);
	}
	first_us = ((const AxisAnalysisPoint *)g_ptr_array_index(points, 0))->taken_us;
	last_us = ((const AxisAnalysisPoint *)g_ptr_array_index(points, points->len - 1))->taken_us;

	set_class(axis, "chart-axis");
	set_int(axis, "x1", PLOT_LEFT);
	set_int(axis, "y1", PLOT_BOTTOM);
	set_int(axis, "x2", PLOT_RIGHT);
	set_int(axis, "y2", PLOT_BOTTOM);
	BROWSER_ADD(svg, axis);
	/* The line is drawn under the points; its coordinates are set below
	 * (the parent holds the element). */
	set_class(line, "chart-line");
	set_attr(line, "fill", "none");
	BROWSER_ADD(svg, line);

	top_label = axis_web_browser_format_size((guint64)largest);
	BROWSER_ADD(svg, svg_text("chart-value", 0, PLOT_TOP + 4, top_label));
	BROWSER_ADD(svg, svg_text("chart-value", 0, PLOT_BOTTOM, "0"));
	first_day = day_of(first_us);
	last_day = day_of(last_us);
	BROWSER_ADD(svg, svg_text("chart-value", PLOT_LEFT, PLOT_BOTTOM + 28, first_day));
	if (points->len > 1) {
		HtmxNode *end = svg_text("chart-value chart-value-end", PLOT_RIGHT, PLOT_BOTTOM + 28, last_day);

		BROWSER_ADD(svg, end);
	}

	step = (points->len + MAX_POINTS - 1) / MAX_POINTS;
	for (i = 0; i < points->len; i++) {
		const AxisAnalysisPoint *point = g_ptr_array_index(points, i);
		gint x;
		gint y;

		if (i % step != 0 && i + 1 != points->len)
			continue;
		if (last_us > first_us)
			x = PLOT_LEFT + (gint)((gdouble)(point->taken_us - first_us) * (PLOT_RIGHT - PLOT_LEFT) /
				(gdouble)(last_us - first_us) + 0.5);
		else
			x = (PLOT_LEFT + PLOT_RIGHT) / 2;
		y = PLOT_BOTTOM;
		if (largest > 0)
			y = PLOT_BOTTOM - (gint)((gdouble)point->bytes * (PLOT_BOTTOM - PLOT_TOP) / (gdouble)largest + 0.5);
		y = CLAMP(y, PLOT_TOP, PLOT_BOTTOM);
		g_string_append_printf(coordinates, "%s%d,%d", coordinates->len > 0 ? " " : "", x, y);
		{
			HtmxNode *dot = svg_new("circle");
			g_autofree gchar *day = day_of(point->taken_us);
			g_autofree gchar *size = axis_web_browser_format_size((guint64)point->bytes);
			g_autofree gchar *hover = g_strdup_printf("%s: %s", day, size);

			set_class(dot, "chart-point");
			set_int(dot, "cx", x);
			set_int(dot, "cy", y);
			set_int(dot, "r", 3);
			BROWSER_ADD(dot, svg_title(hover));
			BROWSER_ADD(svg, dot);
		}
	}
	set_attr(line, "points", coordinates->str);
	g_string_free(coordinates, TRUE);
	return svg;
}
