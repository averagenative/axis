/*
 * axis-web-compat-savings.c - the verdicts and the recommendation as the
 * Savings pages show them
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The Savings pages are for every logged-in user, so what is shown here is
 * counts and the names of libraries and targets only: no device, no
 * account, no play.  An administrator gets a link to the Compatibility
 * pages, where the devices behind each verdict are.
 */

#include "web/axis-web-compat.h"

#include <string.h>

#include "web/axis-web-analysis-private.h"

static gchar *
size_text(gint64 bytes)
{
	return axis_web_browser_format_size((guint64)MAX(bytes, 0));
}

static HtmxNode *
verdict_badge(AxisCompatVerdict verdict)
{
	return axis_web_browser_badge(axis_compat_verdict_label(verdict),
		verdict == AXIS_VERDICT_SAFE ? "badge-succeeded" :
		(verdict == AXIS_VERDICT_RISKY ? "badge-failed" : "badge-warn"));
}

HtmxNode *
axis_web_compat_target_cell(
	const AxisCompatReport *report,
	const gchar            *target_id
){
	HtmxTd *cell = htmx_td_new();
	guint safe;
	guint risky;
	guint unknown;
	guint total;
	g_autofree gchar *text = NULL;

	if (report == NULL) {
		htmx_node_set_text_content(HTMX_NODE(cell), "not available");
		return HTMX_NODE(cell);
	}
	axis_compat_report_target_counts(report, target_id, &safe, &risky, &unknown);
	total = safe + risky + unknown;
	if (total == 0) {
		htmx_node_set_text_content(HTMX_NODE(cell), "no media server library is mapped for it");
		return HTMX_NODE(cell);
	}
	text = g_strdup_printf("%u of %u media server librar%s: %u safe, %u risky, %u unknown", safe, total,
		total == 1 ? "y" : "ies",
		safe, risky, unknown);
	BROWSER_ADD(cell, verdict_badge(risky > 0 ? AXIS_VERDICT_RISKY : (unknown > 0 ? AXIS_VERDICT_UNKNOWN :
		AXIS_VERDICT_SAFE)));
	BROWSER_ADD(cell, axis_web_storage_text("help", text));
	return HTMX_NODE(cell);
}

HtmxNode *
axis_web_compat_recommendation_card(
	const AxisCompatReport *report,
	gboolean                admin
){
	static const AxisWebColumn columns[] = {
		{ "Media server library", AXIS_WEB_COL_NAME },
		{ "Recommendation", AXIS_WEB_COL_WIDE },
		{ "Why", AXIS_WEB_COL_WIDE },
		{ NULL, 0 }
	};
	HtmxNode *card = axis_web_storage_card("Recommended conversion profile per media server library");
	HtmxNode *table;
	HtmxTbody *body;
	guint unmapped = 0;
	guint rows = 0;
	guint i;

	if (report == NULL) {
		BROWSER_ADD(card, axis_web_storage_text("help", "Compatibility is not available."));
		return card;
	}
	BROWSER_ADD(card, axis_web_storage_text("help", "The conversion profile with the largest saving that every "
		"device which plays this media server library has been seen playing directly. A device never seen with it "
		"makes it unknown, and unknown is never recommended: a conversion cannot be undone once the original is "
		"gone."));
	table = axis_web_table_new("recommendations", columns);
	body = htmx_tbody_new();
	for (i = 0; i < report->libraries->len; i++) {
		const AxisCompatLibraryReport *l = g_ptr_array_index(report->libraries, i);
		HtmxTr *row;
		HtmxTd *name;
		HtmxTd *what;
		HtmxTd *why;
		g_autofree gchar *text = NULL;
		const AxisCompatTargetVerdict *tv;

		if (l->state != AXIS_MAP_MAPPED) {
			unmapped++;
			continue;
		}
		row = htmx_tr_new();
		name = htmx_td_new();
		what = htmx_td_new();
		why = htmx_td_new();
		htmx_node_set_text_content(HTMX_NODE(name), l->title != NULL ? l->title : l->key);
		if (l->recommended >= 0) {
			g_autofree gchar *saving = NULL;
			g_autofree gchar *label = NULL;

			tv = g_ptr_array_index(l->targets, (guint)l->recommended);
			saving = size_text(tv->saving);
			label = axis_web_analysis_profile_label(tv->target.family_name, tv->target.name);
			htmx_node_set_text_content(HTMX_NODE(what), label);
			text = l->n_devices == 1 ? g_strdup_printf("Safe for the one device that plays it; saves about %s.",
				saving) : g_strdup_printf("Safe for all %u devices that play it; saves about %s.", l->n_devices,
				saving);
			htmx_node_set_text_content(HTMX_NODE(why), text);
		} else if (l->best >= 0) {
			guint blocked;
			g_autofree gchar *best_name = NULL;

			tv = g_ptr_array_index(l->targets, (guint)l->best);
			best_name = axis_web_analysis_profile_label(tv->target.family_name, tv->target.name);
			blocked = tv->n_risky + tv->n_unknown;
			htmx_node_set_text_content(HTMX_NODE(what), "No profile is safe yet");
			text = g_strdup_printf("The conversion profile that would save most (%s) is held back by %u device%s not "
				"known to play it.", best_name, blocked, blocked == 1 ? "" : "s");
			htmx_node_set_text_content(HTMX_NODE(why), text);
		} else if (l->targets->len > 0) {
			htmx_node_set_text_content(HTMX_NODE(what), "No profile");
			htmx_node_set_text_content(HTMX_NODE(why), "No conversion profile is estimated to save anything here "
				"yet.");
		} else {
			htmx_node_set_text_content(HTMX_NODE(what), "No profile");
			htmx_node_set_text_content(HTMX_NODE(why), "No conversion profile applies to the library folders this "
				"media server library maps to.");
		}
		BROWSER_ADD(row, name);
		BROWSER_ADD(row, what);
		BROWSER_ADD(row, why);
		BROWSER_ADD(body, row);
		rows++;
	}
	if (rows == 0) {
		g_object_unref(body);
		g_object_unref(table);
		BROWSER_ADD(card, axis_web_storage_text(NULL, "No media server library is mapped to a library folder yet, "
			"so nothing is recommended: every verdict is unknown."));
		if (admin)
			BROWSER_ADD(card, axis_web_browser_link("/compat/mapping", "Map media server libraries to library "
				"folders"));
	} else {
		BROWSER_ADD(table, body);
		BROWSER_ADD(card, table);
	}
	if (unmapped > 0) {
		g_autofree gchar *text = g_strdup_printf("%u media server librar%s %s not mapped and %s left out.", unmapped,
			unmapped == 1 ? "y" : "ies", unmapped == 1 ? "is" : "are", unmapped == 1 ? "is" : "are");

		BROWSER_ADD(card, axis_web_storage_text("help", text));
	}
	if (admin)
		BROWSER_ADD(card, axis_web_browser_link("/compat/verdicts", "The devices behind each verdict"));
	return card;
}

HtmxNode *
axis_web_compat_target_card(
	const AxisCompatReport *report,
	const gchar            *target_id,
	gboolean                admin
){
	static const AxisWebColumn columns[] = {
		{ "Media server library", AXIS_WEB_COL_NAME },
		{ "Playback verdict", AXIS_WEB_COL_STATE },
		{ "Safe devices", AXIS_WEB_COL_COUNT },
		{ "Risky devices", AXIS_WEB_COL_COUNT },
		{ "Unknown devices", AXIS_WEB_COL_COUNT },
		{ NULL, 0 }
	};
	HtmxNode *card = axis_web_storage_card("Plays on your devices, per media server library");
	HtmxNode *table;
	HtmxTbody *body;
	guint rows = 0;
	guint i;

	if (report == NULL) {
		BROWSER_ADD(card, axis_web_storage_text("help", "Compatibility is not available."));
		return card;
	}
	table = axis_web_table_new("target-verdicts", columns);
	body = htmx_tbody_new();
	for (i = 0; i < report->libraries->len; i++) {
		const AxisCompatLibraryReport *l = g_ptr_array_index(report->libraries, i);
		guint t;

		for (t = 0; t < l->targets->len; t++) {
			const AxisCompatTargetVerdict *tv = g_ptr_array_index(l->targets, t);
			g_autofree gchar *safe = NULL;
			g_autofree gchar *risky = NULL;
			g_autofree gchar *unknown = NULL;
			HtmxTr *row;
			HtmxTd *name;
			HtmxTd *verdict;
			HtmxTd *cs;
			HtmxTd *cr;
			HtmxTd *cu;

			if (strcmp(tv->target.id, target_id) != 0)
				continue;
			safe = g_strdup_printf("%u", tv->n_safe);
			risky = g_strdup_printf("%u", tv->n_risky);
			unknown = g_strdup_printf("%u", tv->n_unknown);
			row = htmx_tr_new();
			name = htmx_td_new();
			verdict = htmx_td_new();
			cs = htmx_td_new();
			cr = htmx_td_new();
			cu = htmx_td_new();
			htmx_node_set_text_content(HTMX_NODE(name), l->title != NULL ? l->title : l->key);
			BROWSER_ADD(verdict, verdict_badge(tv->verdict));
			htmx_element_add_class(HTMX_ELEMENT(cs), "num");
			htmx_node_set_text_content(HTMX_NODE(cs), safe);
			htmx_element_add_class(HTMX_ELEMENT(cr), "num");
			htmx_node_set_text_content(HTMX_NODE(cr), risky);
			htmx_element_add_class(HTMX_ELEMENT(cu), "num");
			htmx_node_set_text_content(HTMX_NODE(cu), unknown);
			BROWSER_ADD(row, name);
			BROWSER_ADD(row, verdict);
			BROWSER_ADD(row, cs);
			BROWSER_ADD(row, cr);
			BROWSER_ADD(row, cu);
			BROWSER_ADD(body, row);
			rows++;
		}
	}
	if (rows == 0) {
		g_object_unref(body);
		g_object_unref(table);
		BROWSER_ADD(card, axis_web_storage_text("help", "No media server library is mapped for this conversion "
			"profile, so it has no verdict: nobody knows yet whether your devices can play what it makes."));
		if (admin)
			BROWSER_ADD(card, axis_web_browser_link("/compat/mapping", "Map media server libraries to library "
				"folders"));
	} else {
		BROWSER_ADD(card, axis_web_storage_text("help", "Whether the devices that play each media server library "
			"have been seen playing what this conversion profile makes, directly. Unknown is never safe."));
		BROWSER_ADD(table, body);
		BROWSER_ADD(card, table);
	}
	if (admin)
		BROWSER_ADD(card, axis_web_browser_link("/compat/verdicts", "The devices behind each verdict"));
	return card;
}
