/*
 * axis-web-activity.c - status lines, buttons in hand, and the probe a page
 * follows them live with
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "web/axis-web-activity.h"

#include <string.h>

#include "acquire/axis-acquire.h"
#include "web/axis-web.h"
#include "web/axis-web-hub-topic.h"

static void
add(
	gpointer parent,
	gpointer child
){
	htmx_node_add_child(HTMX_NODE(parent), HTMX_NODE(child));
	g_object_unref(child);
}

gchar *
axis_web_activity_ago(
	gint64 us,
	gint64 now_us
){
	gint64 s = (now_us - us) / G_USEC_PER_SEC;

	if (us <= 0)
		return g_strdup("some time ago");
	if (s < 2)
		return g_strdup("a moment ago");
	if (s < 60)
		return g_strdup_printf("%" G_GINT64_FORMAT " seconds ago", s);
	if (s < 120)
		return g_strdup("a minute ago");
	if (s < 3600)
		return g_strdup_printf("%" G_GINT64_FORMAT " minutes ago", s / 60);
	if (s < 7200)
		return g_strdup("an hour ago");
	if (s < 86400)
		return g_strdup_printf("%" G_GINT64_FORMAT " hours ago", s / 3600);
	return axis_web_activity_clock(us, now_us);
}

gchar *
axis_web_activity_clock(
	gint64 us,
	gint64 now_us
){
	g_autoptr(GDateTime) when = us > 0 ? g_date_time_new_from_unix_utc(us / G_USEC_PER_SEC) : NULL;
	g_autoptr(GDateTime) now = g_date_time_new_from_unix_utc(now_us / G_USEC_PER_SEC);

	if (when == NULL)
		return g_strdup("an unknown time");
	if (now != NULL && g_date_time_get_year(when) == g_date_time_get_year(now) &&
	    g_date_time_get_day_of_year(when) == g_date_time_get_day_of_year(now))
		return g_date_time_format(when, "%H:%M UTC");
	return g_date_time_format(when, "%Y-%m-%d %H:%M UTC");
}

gchar *
axis_web_activity_topic(const gchar *key)
{
	g_auto(GStrv) parts = key != NULL ? g_strsplit(key, ":", 8) : NULL;
	GString *topic;
	guint i;

	if (parts == NULL)
		return NULL;
	topic = g_string_new("act");
	for (i = 0; parts[i] != NULL; i++) {
		const gchar *p = parts[i];
		gboolean number = p[0] != '\0';
		guint k;

		for (k = 0; p[k] != '\0'; k++)
			number = number && g_ascii_isdigit(p[k]);
		g_string_append_printf(topic, ":%s", p);
		if (number)
			break;
	}
	if (parts[i] == NULL || !axis_web_hub_topic_is_valid(topic->str)) {
		g_string_free(topic, TRUE);
		return NULL;
	}
	return g_string_free(topic, FALSE);
}

gchar *
axis_web_activity_title_topic(gint64 title_id)
{
	return g_strdup_printf("act:title:%" G_GINT64_FORMAT, title_id);
}

gchar *
axis_web_activity_state(
	const AxisJobLive *job,
	gint64             now_us
){
	GString *text = g_string_new(NULL);

	if (job->running) {
		g_autofree gchar *ago = axis_web_activity_ago(job->started_us, now_us);

		g_string_append_printf(text, "running, started %s", ago);
		if (job->message != NULL && job->message[0] != '\0')
			g_string_append_printf(text, ": %s", job->message);
		else if (job->progress > 0 && job->progress < 1)
			g_string_append_printf(text, ", %d%% done", (gint)(job->progress * 100 + 0.5));
	} else if (job->run_at_us > now_us + G_USEC_PER_SEC) {
		g_autofree gchar *at = axis_web_activity_clock(job->run_at_us, now_us);

		g_string_append_printf(text, "waiting to try again at %s", at);
	} else {
		g_autofree gchar *ago = axis_web_activity_ago(job->queued_us, now_us);

		g_string_append_printf(text, "waiting for its turn, queued %s", ago);
		if (job->ahead == 1)
			g_string_append(text, ", 1 ahead of it");
		else if (job->ahead > 1)
			g_string_append_printf(text, ", %u ahead of it", job->ahead);
		else
			g_string_append(text, ", next in line");
	}
	return g_string_free(text, FALSE);
}

gchar *
axis_web_activity_outcome(
	const gchar          *outcome,
	const gchar          *outcome_text,
	guint                 n_found,
	AxisWebActivityTone  *tone
){
	g_autofree gchar *found = n_found == 1 ? g_strdup("1 release") : g_strdup_printf("%u releases", n_found);

	*tone = AXIS_WEB_ACTIVITY_DONE;
	if (g_strcmp0(outcome, "grabbed") == 0)
		return g_strdup_printf("found %s and sent one to a download client", found);
	if (g_strcmp0(outcome, "listed") == 0)
		return g_strdup_printf("found %s, listed for you to choose from", found);
	*tone = AXIS_WEB_ACTIVITY_NOTHING;
	if (g_strcmp0(outcome, "nothing_found") == 0)
		return g_strdup("found nothing");
	if (g_strcmp0(outcome, "none_acceptable") == 0)
		return g_strdup_printf("found %s, none acceptable under the quality profile", found);
	if (g_strcmp0(outcome, "exhausted") == 0)
		return g_strdup_printf("found %s, none that has not failed before", found);
	if (g_strcmp0(outcome, "in_flight") == 0)
		return g_strdup("took nothing: a grab of it is in flight");
	*tone = AXIS_WEB_ACTIVITY_FAILED;
	if (outcome_text != NULL && outcome_text[0] != '\0') {
		g_autofree gchar *text = g_strdup(outcome_text);
		gsize length = strlen(text);

		if (length > 0 && text[length - 1] == '.')
			text[length - 1] = '\0';
		return g_strdup_printf("did not finish: %s", text);
	}
	return g_strdup("did not finish");
}

HtmxNode *
axis_web_activity_line(
	const gchar         *id,
	AxisWebActivityTone  tone,
	const gchar         *text
){
	static const gchar *const tones[] = { "running", "waiting", "done", "nothing", "failed" };
	HtmxP *line = htmx_p_new();
	g_autofree gchar *css = g_strdup_printf("activity-%s", (guint)tone < G_N_ELEMENTS(tones) ? tones[tone] :
		"done");

	if (id != NULL)
		htmx_element_set_id(HTMX_ELEMENT(line), id);
	htmx_element_add_class(HTMX_ELEMENT(line), "activity-line");
	if (text == NULL) {
		htmx_element_set_attribute(HTMX_ELEMENT(line), "hidden", "hidden");
		return HTMX_NODE(line);
	}
	htmx_element_add_class(HTMX_ELEMENT(line), css);
	htmx_element_set_attribute(HTMX_ELEMENT(line), "role", "status");
	htmx_node_set_text_content(HTMX_NODE(line), text);
	return HTMX_NODE(line);
}

HtmxNode *
axis_web_activity_busy_button(
	const gchar *label,
	const gchar *title
){
	HtmxButton *button = htmx_button_new_with_label(label);

	htmx_element_set_attribute(HTMX_ELEMENT(button), "type", "button");
	htmx_element_set_attribute(HTMX_ELEMENT(button), "disabled", "disabled");
	htmx_element_add_class(HTMX_ELEMENT(button), "button-small");
	htmx_element_add_class(HTMX_ELEMENT(button), "button-busy");
	if (title != NULL)
		htmx_element_set_attribute(HTMX_ELEMENT(button), "title", title);
	return HTMX_NODE(button);
}

gchar *
axis_web_activity_trigger(
	gboolean in_hand,
	gint64   wake_s
){
	GString *trigger = g_string_new("sse:" AXIS_WEB_ACTIVITY_EVENT " delay:400ms, htmx:sseOpen from:body");

	if (in_hand)
		g_string_append_printf(trigger, ", every %ds", AXIS_WEB_ACTIVITY_SAFETY_S);
	if (wake_s > 0)
		g_string_append_printf(trigger, ", load delay:%" G_GINT64_FORMAT "s", wake_s);
	return g_string_free(trigger, FALSE);
}

gint64
axis_web_activity_wake(
	gint64 ended_us,
	gint64 now_us
){
	gint64 left = ended_us + (gint64)AXIS_ACQUIRE_SEARCH_AGAIN_S * G_USEC_PER_SEC - now_us;

	/* A second over, so that the page asks after the server agrees. */
	if (ended_us <= 0 || left <= 0)
		return 0;
	return left / G_USEC_PER_SEC + 2;
}

gint64
axis_web_activity_sooner(
	gint64 a,
	gint64 b
){
	if (a <= 0)
		return b;
	if (b <= 0)
		return a;
	return MIN(a, b);
}

HtmxNode *
axis_web_activity_probe(
	HtmxRequest        *request,
	const gchar        *url,
	const gchar *const *topics,
	const gchar        *trigger
){
	HtmxDiv *probe = htmx_div_new();
	guint i;

	for (i = 0; request != NULL && topics != NULL && topics[i] != NULL; i++)
		axis_web_page_add_topic(request, topics[i]);
	htmx_element_set_id(HTMX_ELEMENT(probe), AXIS_WEB_ACTIVITY_PROBE_ID);
	htmx_element_add_class(HTMX_ELEMENT(probe), "activity-probe");
	htmx_element_set_attribute(HTMX_ELEMENT(probe), "hidden", "hidden");
	htmx_element_set_attribute(HTMX_ELEMENT(probe), "hx-get", url);
	htmx_element_set_attribute(HTMX_ELEMENT(probe), "hx-trigger", trigger);
	htmx_element_set_attribute(HTMX_ELEMENT(probe), "hx-swap", "none");
	return HTMX_NODE(probe);
}

void
axis_web_activity_add_oob(
	HtmxNode *fragment,
	HtmxNode *node
){
	htmx_element_set_attribute(HTMX_ELEMENT(node), "hx-swap-oob", "true");
	add(fragment, node);
}

HtmxResponse *
axis_web_activity_respond(HtmxNode *fragment)
{
	HtmxResponse *response = htmx_response_new();
	g_autofree gchar *markup = htmx_element_render(HTMX_ELEMENT(fragment));

	htmx_response_set_content(response, markup);
	htmx_response_set_content_type(response, "text/html; charset=utf-8");
	htmx_response_add_header(response, "Cache-Control", "no-store");
	return response;
}
