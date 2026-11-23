/*
 * axis-web-storage-view.c - pieces of the storage pages
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Every name and path shown is set as text content or an attribute value,
 * which htmx-glib escapes; nothing is assembled as markup by hand.
 */

#include "web/axis-web-storage-private.h"

#include <stdarg.h>
#include <string.h>

#include "storage/axis-storage-error.h"

#define ADD axis_web_storage_add

void
axis_web_storage_add(
	gpointer parent,
	gpointer child
){
	htmx_node_add_child(HTMX_NODE(parent), HTMX_NODE(child));
	g_object_unref(child);
}

HtmxNode *
axis_web_storage_card(const gchar *heading)
{
	HtmxSection *section = htmx_section_new();
	HtmxHeading *title = htmx_h2_new();

	htmx_element_add_class(HTMX_ELEMENT(section), "card");
	htmx_node_set_text_content(HTMX_NODE(title), heading);
	ADD(section, title);

	return HTMX_NODE(section);
}

HtmxNode *
axis_web_storage_text(
	const gchar *css_class,
	const gchar *text
){
	HtmxP *p = htmx_p_new();

	if (css_class != NULL)
		htmx_element_add_class(HTMX_ELEMENT(p), css_class);
	htmx_node_set_text_content(HTMX_NODE(p), text);

	return HTMX_NODE(p);
}

gchar *
axis_web_storage_format_time(
	gint64       us,
	const gchar *never
){
	return axis_web_format_time(us, never);
}

static gchar *
size_text(guint64 bytes)
{
	return axis_size_format(bytes);
}

static gchar *
element_id(
	gint64       id,
	const gchar *part
){
	return g_strdup_printf("volume-%" G_GINT64_FORMAT "-%s", id, part);
}

HtmxNode *
axis_web_storage_state_block(const AxisVolumeInfo *volume)
{
	HtmxDiv *block = htmx_div_new();
	HtmxSpan *badge = htmx_span_new();
	g_autofree gchar *id = element_id(volume->id, "state");
	g_autofree gchar *badge_class = g_strdup_printf("state-%s",
		axis_volume_state_to_string(volume->state));

	htmx_element_set_id(HTMX_ELEMENT(block), id);
	htmx_element_add_class(HTMX_ELEMENT(block), "volume-state");
	htmx_element_add_class(HTMX_ELEMENT(badge), "badge");
	htmx_element_add_class(HTMX_ELEMENT(badge), badge_class);
	htmx_node_set_text_content(HTMX_NODE(badge), axis_volume_state_label(volume->state));
	ADD(block, badge);

	if (!axis_volume_state_is_usable(volume->state))
		ADD(block, axis_web_storage_text("volume-reason", axis_volume_reason_describe(volume->reason)));

	return HTMX_NODE(block);
}

HtmxNode *
axis_web_storage_space_block(const AxisVolumeInfo *volume)
{
	HtmxDiv *block = htmx_div_new();
	g_autofree gchar *id = element_id(volume->id, "space");

	htmx_element_set_id(HTMX_ELEMENT(block), id);
	htmx_element_add_class(HTMX_ELEMENT(block), "volume-space");

	if (!volume->has_space) {
		htmx_node_set_text_content(HTMX_NODE(block), "Not known yet");
	} else {
		g_autofree gchar *total = size_text(volume->total_bytes);
		g_autofree gchar *free_text = size_text(volume->free_bytes);
		g_autofree gchar *available = size_text(volume->available_bytes);
		g_autofree gchar *line = NULL;

		if (axis_volume_state_is_usable(volume->state)) {
			line = g_strdup_printf("%s free of %s (%s available to Axis)", free_text, total,
				available);
			htmx_node_set_text_content(HTMX_NODE(block), line);
		} else {
			g_autofree gchar *when = axis_web_storage_format_time(volume->space_checked_us, "never");

			line = g_strdup_printf("Last known: %s free of %s, as of %s", free_text, total, when);
			htmx_node_set_text_content(HTMX_NODE(block), line);
		}
	}

	return HTMX_NODE(block);
}

HtmxNode *
axis_web_storage_checked_block(const AxisVolumeInfo *volume)
{
	HtmxDiv *block = htmx_div_new();
	g_autofree gchar *id = element_id(volume->id, "checked");
	g_autofree gchar *checked = axis_web_storage_format_time(volume->last_checked_us, "not yet");
	g_autofree gchar *line = NULL;

	htmx_element_set_id(HTMX_ELEMENT(block), id);
	htmx_element_add_class(HTMX_ELEMENT(block), "volume-checked");

	if (axis_volume_state_is_usable(volume->state)) {
		line = g_strdup_printf("Checked %s", checked);
	} else {
		g_autofree gchar *seen = axis_web_storage_format_time(volume->last_online_us, "never");

		line = g_strdup_printf("Last seen online %s; checked %s", seen, checked);
	}
	htmx_node_set_text_content(HTMX_NODE(block), line);

	return HTMX_NODE(block);
}

static HtmxNode *
cell_with(HtmxNode *child)
{
	HtmxTd *td = htmx_td_new();

	ADD(td, child);
	return HTMX_NODE(td);
}

static HtmxNode *
volume_row(const AxisVolumeInfo *volume)
{
	HtmxTr *row = htmx_tr_new();
	g_autofree gchar *href = g_strdup_printf("/storage/volumes/%" G_GINT64_FORMAT, volume->id);
	g_autofree gchar *row_id = element_id(volume->id, "row");
	g_autofree gchar *roots = g_strdup_printf("%u", volume->n_roots);
	HtmxA *link = htmx_a_new_with_href(href);
	HtmxTd *fs = htmx_td_new();
	g_autofree gchar *fs_text = NULL;

	htmx_element_set_id(HTMX_ELEMENT(row), row_id);
	htmx_node_set_text_content(HTMX_NODE(link), volume->name);
	ADD(row, cell_with(HTMX_NODE(link)));
	ADD(row, cell_with(HTMX_NODE(htmx_code_new_with_text(volume->path))));
	ADD(row, cell_with(axis_web_storage_state_block(volume)));
	fs_text = volume->fs_type != NULL && volume->fs_type[0] != '\0' ?
		g_strdup_printf("%s%s%s", volume->fs_type, volume->mount_root ? "" : " (not a mount point)",
			volume->has_marker ? "" : " (no marker yet)") :
		g_strdup(volume->has_marker ? "" : "No marker yet");
	htmx_node_set_text_content(HTMX_NODE(fs), fs_text);
	ADD(row, fs);
	ADD(row, cell_with(axis_web_storage_space_block(volume)));
	ADD(row, cell_with(axis_web_storage_checked_block(volume)));
	ADD(row, htmx_td_new_with_text(roots));

	return HTMX_NODE(row);
}

HtmxNode *
axis_web_storage_volume_list(
	GPtrArray *volumes,
	gboolean   admin
){
	HtmxDiv *box = htmx_div_new();
	guint i;

	htmx_element_set_id(HTMX_ELEMENT(box), "volume-list");
	/* After a break in the stream, or when volumes come or go, the list
	 * fetches itself again. */
	htmx_element_set_attribute(HTMX_ELEMENT(box), "hx-get", "/storage/list");
	htmx_element_set_attribute(HTMX_ELEMENT(box), "hx-trigger",
		"sse:" AXIS_STORAGE_EVENT_CHANGED " throttle:1s, htmx:sseOpen from:body");
	htmx_element_set_attribute(HTMX_ELEMENT(box), "hx-swap", "outerHTML");

	if (volumes->len == 0) {
		ADD(box, axis_web_storage_text("help", admin ? "No disks yet. Add one with Add a disk below." :
			"No disks yet. An administrator adds disks here."));
	} else {
		static const AxisWebColumn columns[] = {
			{ "Disk", AXIS_WEB_COL_NAME },
			{ "Path", AXIS_WEB_COL_NAME },
			{ "State", AXIS_WEB_COL_STATE },
			{ "Filesystem", AXIS_WEB_COL_TEXT },
			{ "Space", AXIS_WEB_COL_WIDE },
			{ "Checked", AXIS_WEB_COL_WIDE },
			{ "Library folders", AXIS_WEB_COL_COUNT },
			{ NULL, 0 }
		};
		HtmxNode *table = axis_web_table_new("volumes", columns);
		HtmxTbody *body = htmx_tbody_new();

		htmx_element_add_class(HTMX_ELEMENT(table), "volume-table");
		for (i = 0; i < volumes->len; i++)
			ADD(body, volume_row(g_ptr_array_index(volumes, i)));
		ADD(table, body);
		ADD(box, table);
	}

	return HTMX_NODE(box);
}

static void
append_oob(
	GString  *out,
	HtmxNode *node
){
	g_autofree gchar *markup = NULL;

	htmx_element_set_attribute(HTMX_ELEMENT(node), "hx-swap-oob", "true");
	markup = htmx_element_render(HTMX_ELEMENT(node));
	g_string_append(out, markup);
	g_object_unref(node);
}

gchar *
axis_web_storage_render_live(const AxisVolumeInfo *volume)
{
	GString *out = g_string_new(NULL);

	append_oob(out, axis_web_storage_state_block(volume));
	append_oob(out, axis_web_storage_space_block(volume));
	append_oob(out, axis_web_storage_checked_block(volume));

	return g_string_free(out, FALSE);
}

HtmxNode *
axis_web_storage_unmarked_notice(GPtrArray *names)
{
	GString *list;
	g_autofree gchar *text = NULL;
	guint i;

	if (names == NULL || names->len == 0)
		return NULL;

	list = g_string_new(NULL);
	for (i = 0; i < names->len; i++)
		g_string_append_printf(list, "%s'%s'", i == 0 ? "" : i + 1 == names->len ? " and " : ", ",
			(const gchar *)g_ptr_array_index(names, i));
	text = g_strdup_printf("%s %s %s no marker yet, because %s added while %s mounted read-only. A worker "
		"recognises a disk only by its marker, so no worker can use %s: nothing on %s is converted, and no "
		"check or sample measurement of %s files runs on a worker. Axis writes the marker the first time it "
		"finds the disk mounted read-write (see Disks and library folders, the disk's page).",
		names->len == 1 ? "Disk" : "Disks", list->str, names->len == 1 ? "has" : "have",
		names->len == 1 ? "it was" : "they were", names->len == 1 ? "it was" : "they were",
		names->len == 1 ? "it" : "them", names->len == 1 ? "it" : "them", names->len == 1 ? "its" : "their");
	g_string_free(list, TRUE);

	return axis_web_storage_text("notice notice-warning", text);
}

HtmxNode *
axis_web_storage_unmarked_notice_for(AxisStorage *storage)
{
	g_autoptr(GPtrArray) volumes = NULL;
	g_autoptr(GPtrArray) names = g_ptr_array_new();
	guint i;

	if (storage == NULL)
		return NULL;
	volumes = axis_storage_list_volumes(storage);
	for (i = 0; i < volumes->len; i++) {
		const AxisVolumeInfo *volume = g_ptr_array_index(volumes, i);

		if (!volume->has_marker)
			g_ptr_array_add(names, volume->name);
	}
	return axis_web_storage_unmarked_notice(names);
}

gchar *
axis_web_storage_removal_wait_note(
	AxisStorage *storage,
	gint64       volume_id,
	gint64       due_us
){
	g_autoptr(AxisVolumeInfo) volume = NULL;

	if (storage == NULL || due_us <= 0 || due_us > g_get_real_time())
		return NULL;
	if (axis_storage_is_read_only_mode(storage))
		return g_strdup("Waiting: Read-only storage is on (Settings, Read-only storage), and Axis deletes nothing "
			"while it is. The deletion happens at the next tidy-up after you turn it off.");
	volume = volume_id > 0 ? axis_storage_get_volume(storage, volume_id) : NULL;
	if (volume != NULL && volume->state == AXIS_VOLUME_READ_ONLY)
		return g_strdup_printf("Waiting: disk %s is mounted read-only, and Axis deletes nothing from it. The deletion "
			"happens at the next tidy-up after the disk is mounted read-write.", volume->name);
	return NULL;
}

/* ── Form parts ─────────────────────────────────────────────────── */

HtmxNode *
axis_web_storage_text_field(
	const gchar *id,
	const gchar *label,
	const gchar *name,
	const gchar *value,
	const gchar *placeholder,
	const gchar *help
){
	HtmxDiv *field = htmx_div_new();
	HtmxLabel *text = htmx_label_new_for(id);
	HtmxInput *input = htmx_input_new(HTMX_INPUT_TEXT);

	htmx_element_add_class(HTMX_ELEMENT(field), "field");
	htmx_node_set_text_content(HTMX_NODE(text), label);
	htmx_element_set_id(HTMX_ELEMENT(input), id);
	htmx_input_set_name(input, name);
	htmx_element_set_attribute(HTMX_ELEMENT(input), "autocomplete", "off");
	htmx_element_set_attribute(HTMX_ELEMENT(input), "spellcheck", "false");
	if (value != NULL)
		htmx_input_set_value(input, value);
	if (placeholder != NULL)
		htmx_input_set_placeholder(input, placeholder);

	ADD(field, text);
	ADD(field, input);
	if (help != NULL)
		ADD(field, axis_web_storage_text("help", help));

	return HTMX_NODE(field);
}

HtmxNode *
axis_web_storage_checkbox(
	const gchar *id,
	const gchar *label,
	const gchar *name,
	gboolean     required
){
	HtmxDiv *field = htmx_div_new();
	HtmxInput *box = htmx_input_new(HTMX_INPUT_CHECKBOX);
	HtmxLabel *text = htmx_label_new_for(id);

	htmx_element_add_class(HTMX_ELEMENT(field), "field-check");
	htmx_element_set_id(HTMX_ELEMENT(box), id);
	htmx_input_set_name(box, name);
	htmx_input_set_value(box, "yes");
	if (required)
		htmx_input_set_required(box, TRUE);
	htmx_node_set_text_content(HTMX_NODE(text), label);

	ADD(field, box);
	ADD(field, text);

	return HTMX_NODE(field);
}

HtmxNode *
axis_web_storage_media_select(
	const gchar *id,
	const gchar *name
){
	HtmxDiv *field = htmx_div_new();
	HtmxLabel *text = htmx_label_new_for(id);
	HtmxSelect *select = htmx_select_new_with_name(name);
	guint i;

	htmx_element_add_class(HTMX_ELEMENT(field), "field");
	htmx_node_set_text_content(HTMX_NODE(text), "What it holds");
	htmx_element_set_id(HTMX_ELEMENT(select), id);
	for (i = 0; i < AXIS_MEDIA_TYPE_COUNT; i++)
		ADD(select, htmx_option_new_with_value(axis_media_type_to_string((AxisMediaType)i),
			axis_media_type_label((AxisMediaType)i)));

	ADD(field, text);
	ADD(field, select);

	return HTMX_NODE(field);
}

/* ── Requests ───────────────────────────────────────────────────── */

const gchar *
axis_web_storage_done_message(const gchar *key)
{
	static const struct
	{
		const gchar *key;
		const gchar *text;
	} messages[] = {
		{ "added",        "The disk has been added." },
		{ "removed",      "The disk has been removed from Axis. Nothing on it was changed. If it had "
		                  "a marker, the marker is still there, so adding the directory again "
		                  "recognises it." },
		{ "checked",      "The disk has been checked." },
		{ "moved",        "The disk's path has been changed." },
		{ "restored",     "The marker has been written again." },
		{ "root-added",   "The library folder has been added." },
		{ "root-removed", "The library folder has been removed. Nothing on the disk was changed." },
		{ NULL, NULL }
	};
	guint i;

	for (i = 0; key != NULL && messages[i].key != NULL; i++) {
		if (strcmp(key, messages[i].key) == 0)
			return messages[i].text;
	}

	return NULL;
}

gint64
axis_web_storage_param_id(GHashTable *params)
{
	const gchar *text = params != NULL ? g_hash_table_lookup(params, "id") : NULL;
	gint64 value = 0;
	const gchar *p;

	if (text == NULL || text[0] == '\0' || strlen(text) > 18 || text[0] == '0')
		return 0;
	for (p = text; *p != '\0'; p++) {
		if (*p < '0' || *p > '9')
			return 0;
		value = value * 10 + (*p - '0');
	}

	return value;
}

gboolean
axis_web_storage_form_u64(
	HtmxRequest *request,
	const gchar *name,
	guint64     *value
){
	const gchar *text = htmx_request_get_form_value(request, name);
	const gchar *p;
	guint64 result = 0;

	if (text == NULL || text[0] == '\0' || strlen(text) > 20)
		return FALSE;
	for (p = text; *p != '\0'; p++) {
		guint digit;

		if (*p < '0' || *p > '9')
			return FALSE;
		digit = (guint)(*p - '0');
		if (result > (G_MAXUINT64 - digit) / 10)
			return FALSE;
		result = result * 10 + digit;
	}

	*value = result;
	return TRUE;
}

gboolean
axis_web_storage_form_yes(
	HtmxRequest *request,
	const gchar *name
){
	return g_strcmp0(htmx_request_get_form_value(request, name), "yes") == 0;
}

guint
axis_web_storage_error_status(const GError *error)
{
	if (error == NULL || error->domain != AXIS_STORAGE_ERROR)
		return 500;

	switch (error->code) {
	case AXIS_STORAGE_ERROR_INVALID:
		return 400;
	case AXIS_STORAGE_ERROR_NOT_FOUND:
		return 404;
	case AXIS_STORAGE_ERROR_FAILED:
		return 500;
	case AXIS_STORAGE_ERROR_UNAVAILABLE:
	case AXIS_STORAGE_ERROR_READ_ONLY:
	case AXIS_STORAGE_ERROR_REFUSED:
	case AXIS_STORAGE_ERROR_NOT_CONTAINED:
	case AXIS_STORAGE_ERROR_CONFIRM:
	default:
		return 409;
	}
}

void
axis_web_storage_audit(
	HtmxRequest *request,
	const gchar *format,
	...
){
	const AxisAuthState *state = axis_web_auth_state(request);
	g_autofree gchar *text = NULL;
	va_list args;

	va_start(args, format);
	text = g_strdup_vprintf(format, args);
	va_end(args);

	axis_web_auth_audit("%s by=%s client=%s", text, state != NULL ? state->username : "",
		state != NULL ? state->client_address : "");
}
