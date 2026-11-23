/*
 * axis-web-downloads-clients.c - the Download clients pages: the list, one
 * client's state, settings and path mappings, adding, testing, removing
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Administrators only.  A client's secret is never put back in a page: its
 * field is a password field without a value, typed into only to replace
 * the stored secret, and what was typed is wiped once it has been used.  A
 * refused form is shown again with what was typed (but never the secret)
 * and the layer's reason, which is fixed text naming the field, never its
 * value.  Messages after a redirect come from fixed keys.
 */

#include "web/axis-web-downloads-private.h"

#include <string.h>

#include "import/axis-import.h"
#include "web/axis-password.h"
#include "web/axis-web-auth-private.h"
#include "web/axis-web-browser-private.h"
#include "web/axis-web-imports.h"
#include "web/axis-web-notice.h"
#include "web/axis-web-place.h"
#include "web/axis-web-setup-path.h"
#include "web/axis-web-storage-private.h"

#define ADD axis_web_storage_add

/* What a form held, shown again when it is refused (never the secret). */
typedef struct
{
	gchar                *name;
	gchar                *kind;
	gchar                *address;
	gchar                *base_path;
	gchar                *category;
	gchar                *username;	/* NULL: not in the form (a kind without one) */
	AxisDownloadPriority  priority;
	gboolean              accept_untrusted;
	gboolean              enabled;
} Values;

static void
values_clear(Values *values)
{
	g_clear_pointer(&values->name, g_free);
	g_clear_pointer(&values->kind, g_free);
	g_clear_pointer(&values->address, g_free);
	g_clear_pointer(&values->base_path, g_free);
	g_clear_pointer(&values->category, g_free);
	g_clear_pointer(&values->username, g_free);
}

static void
values_from_client(
	Values                       *values,
	const AxisDownloadClientInfo *client
){
	memset(values, 0, sizeof(*values));
	values->name = g_strdup(client->name);
	values->kind = g_strdup(client->kind);
	values->address = g_strdup(client->address);
	values->base_path = g_strdup(client->base_path);
	values->category = g_strdup(client->category);
	values->username = g_strdup(client->username);
	values->priority = client->priority;
	values->accept_untrusted = client->accept_untrusted;
	values->enabled = client->enabled;
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
	g_autofree gchar *priority = form_text(request, "priority", 16);

	memset(values, 0, sizeof(*values));
	values->name = form_text(request, "name", 256);
	values->kind = form_text(request, "kind", 32);
	values->address = form_text(request, "address", 512);
	values->base_path = form_text(request, "base_path", 256);
	values->category = form_text(request, "category", 256);
	values->username = form_text(request, "username", 512);
	if (!axis_download_priority_from_string(priority, &values->priority))
		values->priority = AXIS_DOWNLOAD_PRIORITY_NORMAL;
	values->accept_untrusted = axis_web_storage_form_yes(request, "accept_untrusted");
	values->enabled = axis_web_storage_form_yes(request, "enabled");
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

	/* As axis_web_storage_checkbox(), and ticked when it is on. */
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

/* The secret's field: never a value, only the way to replace it. */
static HtmxNode *
secret_field(
	const gchar *id,
	gboolean     set
){
	HtmxDiv *field = htmx_div_new();
	HtmxLabel *label = htmx_label_new_for(id);
	HtmxInput *input = htmx_input_new(HTMX_INPUT_PASSWORD);

	htmx_element_add_class(HTMX_ELEMENT(field), "field");
	htmx_node_set_text_content(HTMX_NODE(label), "API key or password");
	htmx_element_set_id(HTMX_ELEMENT(input), id);
	htmx_input_set_name(input, "secret");
	htmx_element_set_attribute(HTMX_ELEMENT(input), "autocomplete", "new-password");
	htmx_input_set_placeholder(input, set ? "Type a new value to replace it" : "The client's API key or password");
	ADD(field, label);
	ADD(field, input);
	ADD(field, axis_web_storage_text("help", set ?
		"A value is stored. Type a new one to replace it; leave this empty to keep it. It is never shown again, "
		"never written to a log, and sent only to this client's address." :
		"It is never shown again, never written to a log, and sent only to this client's address."));
	return HTMX_NODE(field);
}

static HtmxNode *
plain_http_warning(void)
{
	return axis_web_storage_text("notice notice-warning", "This address is plain http: the API key or password and "
		"everything else cross the network unencrypted. That is usual inside one machine or a compose network; across "
		"a network, give the download client an https address.");
}

/* The add and edit form's fields, in one stacked form; @prefix makes the
 * fields' ids, @back (nullable) is the setup guide's address it returns to. */
static HtmxNode *
client_form(
	AxisDownloads *downloads,
	HtmxRequest   *request,
	const gchar   *action,
	const Values  *values,
	gboolean       creating,
	gboolean       secret_set,
	const gchar   *button,
	const gchar   *prefix,
	const gchar   *back
){
	static const gchar *const priority_values[] = { "low", "normal", "high", NULL };
	static const gchar *const priority_labels[] = { "Low", "Normal", "High", NULL };
	HtmxNode *form = axis_web_auth_post_form(request, action);
	HtmxNode *back_field = axis_web_setup_return_field(back);
	g_autofree gchar *name_id = g_strdup_printf("%s-name", prefix);
	g_autofree gchar *kind_id = g_strdup_printf("%s-kind", prefix);
	g_autofree gchar *address_id = g_strdup_printf("%s-address", prefix);
	g_autofree gchar *base_path_id = g_strdup_printf("%s-base-path", prefix);
	g_autofree gchar *secret_id = g_strdup_printf("%s-secret", prefix);
	g_autofree gchar *username_id = g_strdup_printf("%s-username", prefix);
	g_autofree gchar *category_id = g_strdup_printf("%s-category", prefix);
	g_autofree gchar *priority_id = g_strdup_printf("%s-priority", prefix);
	g_autofree gchar *untrusted_id = g_strdup_printf("%s-untrusted", prefix);
	g_autofree gchar *enabled_id = g_strdup_printf("%s-enabled", prefix);

	htmx_element_add_class(HTMX_ELEMENT(form), "form");
	ADD(form, axis_web_storage_text_field(name_id, "Name", "name", values->name, "Torrents",
		"The name Axis shows on its pages."));
	if (creating) {
		g_autoptr(GPtrArray) names = g_ptr_array_new();
		g_autoptr(GPtrArray) labels = g_ptr_array_new();
		guint i;

		for (i = 0; i < axis_downloads_n_kinds(downloads); i++) {
			const AxisDownloadKindInfo *kind = axis_downloads_kind_at(downloads, i);

			/* A built-in kind's one client is Axis's own: none is added. */
			if (kind->builtin)
				continue;
			g_ptr_array_add(names, (gpointer)kind->name);
			g_ptr_array_add(labels, (gpointer)kind->label);
		}
		g_ptr_array_add(names, NULL);
		g_ptr_array_add(labels, NULL);
		ADD(form, select_field(kind_id, "Kind", "kind", (const gchar *const *)names->pdata,
			(const gchar *const *)labels->pdata, values->kind));
	}
	ADD(form, axis_web_storage_text_field(address_id, "Address", "address", values->address,
		"http://deluge:8112", "The client's own address: http or https, its host and port. A path goes in the base path below."));
	ADD(form, axis_web_storage_text_field(base_path_id, "Base path (optional)", "base_path", values->base_path,
		"/sabnzbd", "Only when the client is served under a path, as it is behind some reverse proxies: a slash and "
		"the path, such as /sabnzbd. Leave it empty when the client is at the top of its address. It is typed here "
		"and nothing a client says changes it."));
	{
		/* A user name for the kinds whose login takes one: always on the form
		 * that adds a client (the kind is chosen on it, and there is no script
		 * to follow the choice), and on a client's own form only when its
		 * kind has one. */
		const AxisDownloadKindInfo *own = values->kind != NULL ? axis_downloads_find_kind(downloads, values->kind) : NULL;
		g_autoptr(GString) users = g_string_new(NULL);
		guint k;

		for (k = 0; k < axis_downloads_n_kinds(downloads); k++) {
			const AxisDownloadKindInfo *kind = axis_downloads_kind_at(downloads, k);

			if (kind->username_label == NULL)
				continue;
			g_string_append_printf(users, "%s%s", users->len > 0 ? " and " : "", kind->label);
		}
		if (creating ? users->len > 0 : (own != NULL && own->username_label != NULL)) {
			g_autofree gchar *label = creating ? g_strdup_printf("User name (%s only)", users->str) :
				g_strdup(own->username_label);
			g_autofree gchar *help = creating ?
				g_strdup_printf("The kinds that log in with a user name: %s. Leave it empty for the others. It is "
					"not a secret: it is shown on the client's page.", users->str) :
				g_strdup("The user name the client's web interface logs in with. It is not a secret: it is shown on "
					"the client's page.");

			ADD(form, axis_web_storage_text_field(username_id, label, "username", values->username, "admin", help));
		}
	}
	ADD(form, secret_field(secret_id, secret_set));
	ADD(form, axis_web_storage_text_field(category_id, "Category or label", "category",
		values->category != NULL ? values->category : AXIS_DOWNLOAD_DEFAULT_CATEGORY, AXIS_DOWNLOAD_DEFAULT_CATEGORY,
		"Axis gives every download it adds this category (or label), and looks at nothing else in the client. "
		"Keep it apart from what Radarr and Sonarr use."));
	ADD(form, select_field(priority_id, "Priority of its downloads", "priority", priority_values, priority_labels,
		axis_download_priority_to_string(values->priority)));
	ADD(form, checkbox(untrusted_id, "Accept a self-signed certificate", "accept_untrusted",
		values->accept_untrusted));
	ADD(form, checkbox(enabled_id, "Switched on", "enabled", values->enabled));
	ADD(form, axis_web_storage_text("help", "Switched off, it is sent no new downloads; the downloads it has already "
		"are still followed to the end and imported."));
	if (back_field != NULL)
		ADD(form, back_field);
	{
		HtmxNode *submit = axis_web_auth_button(button, "button-primary");

		ADD(form, submit);
	}
	return form;
}

HtmxNode *
axis_web_downloads_add_form(
	AxisDownloads *downloads,
	HtmxRequest   *request,
	const gchar   *return_path
){
	Values fresh;

	memset(&fresh, 0, sizeof(fresh));
	fresh.priority = AXIS_DOWNLOAD_PRIORITY_NORMAL;
	fresh.enabled = TRUE;
	return client_form(downloads, request, "/downloads/clients", &fresh, TRUE, FALSE, "Add the download client",
		"new-client", return_path);
}

HtmxNode *
axis_web_downloads_edit_form(
	AxisDownloads                *downloads,
	HtmxRequest                  *request,
	const AxisDownloadClientInfo *client,
	const gchar                  *return_path
){
	g_autofree gchar *action = g_strdup_printf("/downloads/clients/%" G_GINT64_FORMAT, client->id);
	g_autofree gchar *prefix = g_strdup_printf("client-%" G_GINT64_FORMAT, client->id);
	Values current;
	HtmxNode *form;

	values_from_client(&current, client);
	form = client_form(downloads, request, action, &current, FALSE, client->secret_set, "Save", prefix, return_path);
	values_clear(&current);
	return form;
}

/* The test button, with a name for a screen reader when @aria is given. */
static HtmxNode *
test_form_named(
	HtmxRequest *request,
	gint64       client_id,
	const gchar *label,
	const gchar *css_class,
	const gchar *return_path,
	const gchar *aria
){
	g_autofree gchar *action = g_strdup_printf("/downloads/clients/%" G_GINT64_FORMAT "/test", client_id);
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
axis_web_downloads_test_form(
	HtmxRequest *request,
	gint64       client_id,
	const gchar *label,
	const gchar *css_class,
	const gchar *return_path
){
	return test_form_named(request, client_id, label, css_class, return_path, NULL);
}

HtmxNode *
axis_web_downloads_map_form(
	HtmxRequest *request,
	gint64       client_id,
	GPtrArray   *volumes,
	const gchar *return_path
){
	g_autofree gchar *action = g_strdup_printf("/downloads/clients/%" G_GINT64_FORMAT "/maps", client_id);
	g_autofree gchar *prefix_id = g_strdup_printf("map-%" G_GINT64_FORMAT "-prefix", client_id);
	g_autofree gchar *volume_id = g_strdup_printf("map-%" G_GINT64_FORMAT "-volume", client_id);
	g_autofree gchar *rel_id = g_strdup_printf("map-%" G_GINT64_FORMAT "-rel", client_id);
	g_autoptr(GPtrArray) ids = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GPtrArray) names = g_ptr_array_new();
	HtmxNode *form = axis_web_auth_post_form(request, action);
	HtmxNode *back = axis_web_setup_return_field(return_path);
	guint i;

	for (i = 0; i < volumes->len; i++) {
		const AxisVolumeInfo *volume = g_ptr_array_index(volumes, i);

		g_ptr_array_add(ids, g_strdup_printf("%" G_GINT64_FORMAT, volume->id));
		g_ptr_array_add(names, volume->name);
	}
	g_ptr_array_add(ids, NULL);
	g_ptr_array_add(names, NULL);
	htmx_element_add_class(HTMX_ELEMENT(form), "inline-form");
	ADD(form, axis_web_storage_text_field(prefix_id, "Client's path", "prefix", NULL, "/downloads/complete", NULL));
	ADD(form, select_field(volume_id, "Disk", "volume", (const gchar *const *)ids->pdata,
		(const gchar *const *)names->pdata, NULL));
	ADD(form, axis_web_storage_text_field(rel_id, "Path on the disk", "rel_path", NULL, "downloads/complete", NULL));
	if (back != NULL)
		ADD(form, back);
	ADD(form, axis_web_auth_button("Add the mapping", NULL));
	return form;
}

/* ── The list ───────────────────────────────────────────────────── */

static const AxisWebColumn client_columns[] = {
	{ "Name", AXIS_WEB_COL_NAME },
	{ "Kind", AXIS_WEB_COL_TEXT },
	{ "Category or label", AXIS_WEB_COL_TEXT },
	{ "State", AXIS_WEB_COL_STATE },
	{ "Last answer", AXIS_WEB_COL_DATE },
	{ "Problem", AXIS_WEB_COL_WIDE },
	{ "", AXIS_WEB_COL_ACTIONS },
	{ NULL, 0 }
};

static const gchar *
kind_label(
	AxisWebDownloads *self,
	const gchar      *name
){
	const AxisDownloadKindInfo *kind = axis_downloads_find_kind(self->downloads, name);

	return kind != NULL ? kind->label : name;
}

static HtmxNode *
client_row(
	AxisWebDownloads             *self,
	HtmxRequest                  *request,
	const AxisDownloadClientInfo *client
){
	HtmxTr *row = htmx_tr_new();
	HtmxTd *name = htmx_td_new();
	HtmxSpan *address = htmx_span_new();
	HtmxTd *problem = htmx_td_new();
	g_autofree gchar *href = g_strdup_printf("/downloads/clients/%" G_GINT64_FORMAT, client->id);
	g_autofree gchar *id = g_strdup_printf("client-%" G_GINT64_FORMAT, client->id);
	g_autofree gchar *contact = axis_web_format_time(client->last_contact_us, "never");
	HtmxA *link = htmx_a_new_with_href(href);
	HtmxBdi *text = htmx_bdi_new();
	GString *why = g_string_new(NULL);

	htmx_element_set_id(HTMX_ELEMENT(row), id);
	htmx_node_set_text_content(HTMX_NODE(text), client->name);
	ADD(link, text);
	ADD(name, link);
	if (!client->enabled)
		ADD(name, axis_web_browser_badge(client->n_active > 0 ? "Off: no new downloads" : "Off", "badge-note"));
	/* Its address under its name, where a long one wraps. */
	htmx_element_add_class(HTMX_ELEMENT(address), "range-detail");
	{
		g_autofree gchar *shown = g_strconcat(client->address, client->base_path != NULL ? client->base_path : "", NULL);
		HtmxCode *code = htmx_code_new();

		ADD(code, axis_web_breakable(shown));
		ADD(address, code);
	}
	if (client->plain_http)
		ADD(address, axis_web_browser_badge("Not encrypted", "badge-warn"));
	ADD(name, address);
	ADD(row, name);
	ADD(row, cell_text(kind_label(self, client->kind)));
	ADD(row, cell_node(HTMX_NODE(htmx_code_new_with_text(client->category))));
	ADD(row, cell_node(axis_web_downloads_client_badge(client->state)));
	ADD(row, cell_text(contact));
	if (client->state != AXIS_DOWNLOAD_CLIENT_OK && client->state_message != NULL)
		g_string_append(why, client->state_message);
	if (!client->secret_set)
		g_string_append_printf(why, "%sNo API key or password is set: add it under Settings for this download client.",
			why->len > 0 ? " " : "");
	if (client->problem != NULL)
		g_string_append_printf(why, "%s%s", why->len > 0 ? " " : "", client->problem);
	htmx_node_set_text_content(HTMX_NODE(problem), why->str);
	g_string_free(why, TRUE);
	ADD(row, problem);
	{
		g_autofree gchar *aria = g_strdup_printf("Test %s", client->name);

		ADD(row, cell_node(test_form_named(request, client->id, "Test", "button-small", NULL, aria)));
	}
	return HTMX_NODE(row);
}

typedef struct
{
	gchar     *done;
	GPtrArray *clients;
	GPtrArray *warnings;
	GError    *error;
} ListWork;

static void
list_work_free(gpointer data)
{
	ListWork *work = data;

	g_free(work->done);
	g_clear_pointer(&work->clients, g_ptr_array_unref);
	g_clear_pointer(&work->warnings, g_ptr_array_unref);
	g_clear_error(&work->error);
	g_free(work);
}

/* The pairs of a client's folder and a films root on different volumes
 * (every import between them would be a copy); @client_id 0 for all. */
static GPtrArray *
volume_warnings(
	AxisWebDownloads *self,
	gint64            client_id
){
	g_autoptr(GPtrArray) all = NULL;
	g_autoptr(GError) error = NULL;
	GPtrArray *mine;

	if (!axis_import_volume_warnings(axis_downloads_get_pool(self->downloads), &all, &error)) {
		g_debug("download clients: the volumes could not be compared: %s", error->message);
		return NULL;
	}
	if (client_id == 0)
		return g_steal_pointer(&all);
	mine = g_ptr_array_new_with_free_func((GDestroyNotify)axis_import_volume_warning_free);
	while (all->len > 0) {
		AxisImportVolumeWarning *warning = g_ptr_array_steal_index(all, 0);

		if (warning->client_id == client_id)
			g_ptr_array_add(mine, warning);
		else
			axis_import_volume_warning_free(warning);
	}
	return mine;
}

static void
list_run(
	AxisWebDownloads *self,
	gpointer          data
){
	ListWork *work = data;

	if (axis_downloads_list_clients(self->downloads, &work->clients, &work->error))
		work->warnings = volume_warnings(self, 0);
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
		{ "added", FALSE, "The download client has been added. Test it, then map its download folders to disks under "
			"Path mappings." },
		{ "saved", FALSE, "The settings have been saved." },
		{ "tested", FALSE, "The download client has been tested: the result is under State of this download client." },
		{ "deleted", FALSE, "The download client has been removed. The history of its downloads stays." },
		{ "map-added", FALSE, "The path mapping has been added." },
		{ "map-removed", FALSE, "The path mapping has been removed." },
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

static HtmxResponse *
list_page(
	AxisWebDownloads *self,
	HtmxRequest      *request,
	GPtrArray        *clients,
	GPtrArray        *warnings,
	const gchar      *notice,
	gboolean          notice_error,
	const Values     *submitted,
	const gchar      *back
){
	g_autoptr(HtmxDiv) content = htmx_div_new();
	HtmxNode *card;
	HtmxNode *warning;
	Values fresh;
	gboolean plain = FALSE;
	guint i;

	ADD(content, heading(1, "Download clients"));
	ADD(content, axis_web_page_intro("The programs that do the downloading, such as SABnzbd for Usenet and Deluge or "
		"qBittorrent for torrents: Axis drives the ones already running, handing them releases, following what it added and "
		"finding the files on a disk when they finish. It looks only at the downloads that carry its category, "
		"and changes or removes only those it added itself."));
	ADD(content, axis_web_storage_text("help", AXIS_IMPORT_TORRENTS_HELP));
	if (notice != NULL)
		ADD(content, axis_web_auth_notice(notice_error, notice));
	warning = axis_web_imports_volume_warnings(warnings);
	if (warning != NULL)
		ADD(content, warning);
	card = axis_web_storage_card("Your download clients");
	if (clients->len == 0) {
		ADD(card, axis_web_storage_text("help", "No download client has been added yet. Add one below, under Add a "
			"download client."));
	} else {
		HtmxNode *table = axis_web_table_new("download-clients", client_columns);
		HtmxTbody *body = htmx_tbody_new();

		for (i = 0; i < clients->len; i++) {
			const AxisDownloadClientInfo *client = g_ptr_array_index(clients, i);

			plain = plain || client->plain_http;
			ADD(body, client_row(self, request, client));
		}
		ADD(table, body);
		ADD(card, table);
		if (plain)
			ADD(card, axis_web_storage_text("help", "\"Not encrypted\": the client's address is plain http. That "
				"is usual inside one machine or a compose network; across a network, use https."));
	}
	ADD(content, card);

	card = axis_web_storage_card("Add a download client");
	if (axis_downloads_n_kinds(self->downloads) == 0) {
		ADD(card, axis_web_storage_text("help", "This version of Axis has no kind of download client built in."));
	} else {
		HtmxNode *note = axis_web_setup_back_note(back);

		if (note != NULL)
			ADD(card, note);
		memset(&fresh, 0, sizeof(fresh));
		fresh.priority = AXIS_DOWNLOAD_PRIORITY_NORMAL;
		fresh.enabled = TRUE;
		ADD(card, client_form(self->downloads, request, "/downloads/clients", submitted != NULL ? submitted : &fresh,
			TRUE, FALSE, "Add the download client", "new-client", back));
	}
	ADD(content, card);
	return axis_web_page_respond(request, "Download clients", HTMX_NODE(content));
}

static HtmxResponse *
list_done(
	AxisWebDownloads *self,
	HtmxRequest      *request,
	gpointer          data
){
	ListWork *work = data;
	gboolean is_error = FALSE;
	const gchar *message;

	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	message = done_message(work->done, &is_error);
	return list_page(self, request, work->clients, work->warnings, message, is_error, NULL, NULL);
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
	return axis_web_downloads_dispatch(user_data, request, list_run, list_done, work, list_work_free);
}

/* ── One client ─────────────────────────────────────────────────── */

typedef struct
{
	gint64                  id;
	gchar                  *done;
	AxisDownloadClientInfo *client;
	GPtrArray              *warnings;
	GError                 *error;
} ClientWork;

static void
client_work_free(gpointer data)
{
	ClientWork *work = data;

	g_free(work->done);
	g_clear_pointer(&work->client, axis_download_client_info_free);
	g_clear_pointer(&work->warnings, g_ptr_array_unref);
	g_clear_error(&work->error);
	g_free(work);
}

static void
client_run(
	AxisWebDownloads *self,
	gpointer          data
){
	ClientWork *work = data;

	if (axis_downloads_get_client(self->downloads, work->id, &work->client, &work->error))
		work->warnings = volume_warnings(self, work->id);
}

static const AxisWebColumn map_columns[] = {
	{ "Client's path", AXIS_WEB_COL_NAME },
	{ "Disk", AXIS_WEB_COL_TEXT },
	{ "Path on the disk", AXIS_WEB_COL_WIDE },
	{ "", AXIS_WEB_COL_ACTIONS },
	{ NULL, 0 }
};

static HtmxNode *
maps_card(
	AxisWebDownloads             *self,
	HtmxRequest                  *request,
	const AxisDownloadClientInfo *client,
	const gchar                  *back,
	AxisWebNotice                *placed
){
	HtmxNode *card = axis_web_storage_card("Path mappings");
	g_autofree gchar *remove = g_strdup_printf("/downloads/clients/%" G_GINT64_FORMAT "/maps/remove", client->id);
	g_autoptr(GPtrArray) volumes = axis_storage_list_volumes(self->storage);
	guint i;

	htmx_element_set_id(HTMX_ELEMENT(card), "client-maps");
	axis_web_notice_put(placed, card, "client-maps");
	ADD(card, axis_web_storage_text(NULL, "The download client names files by its own paths, which are those of "
		"its container. Map each of its download folders to the place on a disk where Axis sees the same files. Axis "
		"imports a finished download only when its path maps to a folder on a disk and the files are found there; "
		"otherwise the download waits and the page shows why. Axis never guesses a mapping."));
	if (client->maps->len == 0) {
		ADD(card, axis_web_storage_text("help", "No path is mapped yet. Add a mapping below; until then, finished "
			"downloads wait."));
	} else {
		HtmxNode *table = axis_web_table_new("download-maps", map_columns);
		HtmxTbody *body = htmx_tbody_new();

		for (i = 0; i < client->maps->len; i++) {
			const AxisDownloadPathMapInfo *map = g_ptr_array_index(client->maps, i);
			HtmxTr *row = htmx_tr_new();
			HtmxNode *drop = axis_web_auth_post_form(request, remove);

			ADD(row, cell_node(HTMX_NODE(htmx_code_new_with_text(map->client_prefix))));
			ADD(row, cell_text(map->volume_name));
			/* Text, not code: a long path wraps in this column. */
			ADD(row, cell_text(map->rel_path[0] != '\0' ? map->rel_path : "(the top of the disk)"));
			htmx_element_add_class(HTMX_ELEMENT(drop), "inline-form");
			ADD(drop, axis_web_auth_hidden("prefix", map->client_prefix));
			{
				g_autofree gchar *aria = g_strdup_printf("Remove the mapping of %s", map->client_prefix);
				HtmxNode *button = axis_web_auth_button("Remove mapping", "button-small");

				htmx_element_set_attribute(HTMX_ELEMENT(button), "aria-label", aria);
				ADD(drop, button);
			}
			ADD(row, cell_node(drop));
			ADD(body, row);
		}
		ADD(table, body);
		ADD(card, table);
	}
	if (volumes->len == 0) {
		HtmxP *line = htmx_p_new();
		HtmxSpan *words = htmx_span_new();
		HtmxSpan *tail = htmx_span_new();

		htmx_element_add_class(HTMX_ELEMENT(line), "help");
		htmx_node_set_text_content(HTMX_NODE(words), "No disk has been added yet. Add one on the ");
		htmx_node_set_text_content(HTMX_NODE(tail), " page first.");
		ADD(line, words);
		ADD(line, axis_web_browser_link("/storage", "Disks"));
		ADD(line, tail);
		ADD(card, line);
		return card;
	}
	ADD(card, heading(3, "Add a mapping"));
	if (back != NULL)
		ADD(card, axis_web_setup_back_note(back));
	ADD(card, axis_web_downloads_map_form(request, client->id, volumes, back));
	return card;
}

static HtmxResponse *
client_page(
	AxisWebDownloads             *self,
	HtmxRequest                  *request,
	const AxisDownloadClientInfo *client,
	GPtrArray                    *warnings,
	const gchar                  *notice,
	gboolean                      notice_error,
	const Values                 *submitted,
	guint                         status,
	const gchar                  *back,
	gboolean                      back_to_maps
){
	HtmxNode *warning = axis_web_imports_volume_warnings(warnings);
	g_autoptr(HtmxDiv) content = htmx_div_new();
	HtmxNode *card = axis_web_storage_card("State of this download client");
	HtmxNode *facts = axis_web_browser_facts_new();
	g_autofree gchar *action = g_strdup_printf("/downloads/clients/%" G_GINT64_FORMAT, client->id);
	g_autofree gchar *delete = g_strdup_printf("/downloads/clients/%" G_GINT64_FORMAT "/delete", client->id);
	g_autofree gchar *checked = axis_web_format_time(client->last_checked_us, "never");
	g_autofree gchar *contact = axis_web_format_time(client->last_contact_us, "never");
	g_autofree gchar *active = g_strdup_printf("%u", client->n_active);
	HtmxResponse *response;
	HtmxNode *form;
	AxisWebNotice placed;
	Values current;

	ADD(content, heading(1, client->name));
	axis_web_notice_begin(&placed, request, notice_error, g_strdup(notice), HTMX_NODE(content));
	htmx_element_set_id(HTMX_ELEMENT(card), "client-state");
	axis_web_notice_put(&placed, card, "client-state");

	axis_web_browser_fact(facts, "Kind", kind_label(self, client->kind));
	{
		const AxisDownloadKindInfo *own = axis_downloads_find_kind(self->downloads, client->kind);

		if (own != NULL && own->username_label != NULL)
			axis_web_browser_fact(facts, own->username_label, client->username != NULL ? client->username :
				"not set (admin is used)");
	}
	axis_web_browser_fact_node(facts, "State", axis_web_downloads_client_badge(client->state));
	if (client->state_message != NULL)
		axis_web_browser_fact(facts, client->state == AXIS_DOWNLOAD_CLIENT_OK ? "Note" :
			"What the download client said", client->state_message);
	axis_web_browser_fact(facts, "Version", client->version != NULL ? client->version : "not known yet");
	axis_web_browser_fact(facts, "Category or label in the download client",
		client->category_exists < 0 ? "not known yet" : client->category_exists ? "exists" :
		"does not exist in the download client: see What the download client said, above");
	axis_web_browser_fact(facts, "Last looked at", checked);
	axis_web_browser_fact(facts, "Last answered", contact);
	axis_web_browser_fact(facts, "Downloads in progress", active);
	if (client->problem != NULL)
		axis_web_browser_fact_node(facts, "Problem", axis_web_browser_warning(client->problem));
	ADD(card, facts);
	ADD(card, axis_web_downloads_test_form(request, client->id, "Test this download client now", NULL, NULL));
	ADD(content, card);

	card = axis_web_storage_card("Settings for this download client");
	htmx_element_set_id(HTMX_ELEMENT(card), "client-settings");
	axis_web_notice_put(&placed, card, "client-settings");
	if (client->plain_http)
		ADD(card, plain_http_warning());
	if (back != NULL && !back_to_maps)
		ADD(card, axis_web_setup_back_note(back));
	if (submitted == NULL)
		values_from_client(&current, client);
	ADD(card, client_form(self->downloads, request, action, submitted != NULL ? submitted : &current, FALSE,
		client->secret_set, "Save", "client", back_to_maps ? NULL : back));
	if (submitted == NULL)
		values_clear(&current);
	ADD(content, card);

	if (warning != NULL)
		ADD(content, warning);
	ADD(content, maps_card(self, request, client, back_to_maps ? back : NULL, &placed));

	card = axis_web_storage_card("Remove this download client");
	ADD(card, axis_web_storage_text(NULL, "Axis forgets the download client, its API key or password, and its path "
		"mappings. Nothing in the download client is changed; the history of its downloads stays here under its name. "
		"A download client with downloads in progress cannot be removed."));
	form = axis_web_auth_post_form(request, delete);
	htmx_element_add_class(HTMX_ELEMENT(form), "inline-form");
	ADD(form, axis_web_storage_checkbox("client-delete-confirm", "Remove it for good", "confirm", TRUE));
	ADD(form, axis_web_auth_button("Remove this download client", "button-danger"));
	ADD(card, form);
	ADD(content, card);
	ADD(content, axis_web_browser_link("/downloads/clients", "Back to Download clients"));
	axis_web_notice_end(&placed);
	response = axis_web_page_respond(request, client->name, HTMX_NODE(content));
	if (status != 200)
		htmx_response_set_status(response, status);
	return response;
}

static HtmxResponse *
client_done(
	AxisWebDownloads *self,
	HtmxRequest      *request,
	gpointer          data
){
	ClientWork *work = data;
	gboolean is_error = FALSE;
	const gchar *message;

	if (g_error_matches(work->error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_NOT_FOUND))
		return axis_web_error_respond(request, 404, "Not found", "There is no such download client.");
	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	message = done_message(work->done, &is_error);
	return client_page(self, request, work->client, work->warnings, message, is_error, NULL, 200, NULL, FALSE);
}

static HtmxResponse *
handle_client(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	gint64 id = axis_web_storage_param_id(params);
	const gchar *done = htmx_request_get_query_param(request, "done");
	gboolean is_error = FALSE;
	ClientWork *work;

	if (id <= 0)
		return axis_web_error_respond(request, 404, "Not found", "There is no such download client.");
	work = g_new0(ClientWork, 1);
	work->id = id;
	if (done_message(done, &is_error) != NULL)
		work->done = g_strdup(done);
	return axis_web_downloads_dispatch(user_data, request, client_run, client_done, work, client_work_free);
}

/* ── Changes ────────────────────────────────────────────────────── */

typedef enum
{
	CHANGE_SAVE,
	CHANGE_TEST,
	CHANGE_ADD_MAP,
	CHANGE_REMOVE_MAP,
	CHANGE_DELETE
} ChangeId;

typedef struct
{
	ChangeId                change;
	gint64                  id;	/* the client; 0 for a new one */
	Values                  values;
	gchar                  *secret;	/* wiped */
	gchar                  *prefix;
	gchar                  *rel_path;
	gint64                  volume_id;
	gint64                  saved_id;
	GError                 *error;
	AxisDownloadClientInfo *client;	/* to show the page again */
	GPtrArray              *clients;	/* the list, to show it again */
	const gchar            *back;	/* the setup guide's address (static), or NULL */
} ChangeWork;

static void
change_work_free(gpointer data)
{
	ChangeWork *work = data;

	values_clear(&work->values);
	axis_secret_free(work->secret);
	g_free(work->prefix);
	g_free(work->rel_path);
	g_clear_error(&work->error);
	g_clear_pointer(&work->client, axis_download_client_info_free);
	g_clear_pointer(&work->clients, g_ptr_array_unref);
	g_free(work);
}

static void
change_run(
	AxisWebDownloads *self,
	gpointer          data
){
	ChangeWork *work = data;
	g_autoptr(GError) reread = NULL;

	switch (work->change) {
	case CHANGE_SAVE: {
		AxisDownloadClientSpec spec;

		memset(&spec, 0, sizeof(spec));
		spec.name = work->values.name;
		spec.kind = work->values.kind;
		spec.address = work->values.address;
		spec.base_path = work->values.base_path;
		spec.accept_untrusted = work->values.accept_untrusted;
		spec.category = work->values.category;
		spec.username = work->values.username;
		spec.priority = work->values.priority;
		spec.enabled = work->values.enabled;
		spec.secret = work->secret;
		axis_downloads_save_client(self->downloads, work->id, &spec, &work->saved_id, &work->error);
		break;
	}
	case CHANGE_TEST:
		axis_downloads_test_client(self->downloads, work->id, NULL, &work->error);
		break;
	case CHANGE_ADD_MAP:
		axis_downloads_add_path_map(self->downloads, work->id, work->prefix, work->volume_id, work->rel_path,
			&work->error);
		break;
	case CHANGE_REMOVE_MAP:
		axis_downloads_remove_path_map(self->downloads, work->id, work->prefix, &work->error);
		break;
	default:
		axis_downloads_delete_client(self->downloads, work->id, &work->error);
		break;
	}
	/* The secret is not needed beyond this. */
	g_clear_pointer(&work->secret, axis_secret_free);
	/* A refusal shows the page again: read what it shows. */
	if (work->error != NULL && work->error->domain == AXIS_DOWNLOAD_ERROR &&
	    !g_error_matches(work->error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_NOT_FOUND)) {
		if (work->id == 0)
			axis_downloads_list_clients(self->downloads, &work->clients, &reread);
		else
			axis_downloads_get_client(self->downloads, work->id, &work->client, &reread);
	}
}

static HtmxResponse *
change_done(
	AxisWebDownloads *self,
	HtmxRequest      *request,
	gpointer          data
){
	ChangeWork *work = data;
	g_autofree gchar *location = NULL;
	const gchar *done = NULL;
	const gchar *place = NULL;

	if (g_error_matches(work->error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_NOT_FOUND))
		return axis_web_error_respond(request, 404, "Not found", "There is no such download client.");
	if (work->error != NULL && work->error->domain == AXIS_DOWNLOAD_ERROR && (work->client != NULL ||
	    work->clients != NULL)) {
		/* The layer's words name the field, never its value. */
		if (work->id == 0) {
			HtmxResponse *response = list_page(self, request, work->clients, NULL, work->error->message, TRUE,
				&work->values, work->back);

			htmx_response_set_status(response, 400);
			return response;
		}
		return client_page(self, request, work->client, NULL, work->error->message, TRUE,
			work->change == CHANGE_SAVE ? &work->values : NULL, 400, work->back,
			work->change == CHANGE_ADD_MAP || work->change == CHANGE_REMOVE_MAP);
	}
	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	switch (work->change) {
	case CHANGE_SAVE:
		axis_web_storage_audit(request, "download client %" G_GINT64_FORMAT " %s", work->saved_id,
			work->id == 0 ? "added" : "changed");
		done = work->id == 0 ? "added" : "saved";
		if (work->back == NULL && work->id != 0) {
			location = g_strdup_printf("/downloads/clients/%" G_GINT64_FORMAT, work->saved_id);
			place = "client-settings";
			break;
		}
		location = work->back != NULL ? axis_web_setup_return_location(work->back, done) :
			g_strdup_printf("/downloads/clients/%" G_GINT64_FORMAT "?done=%s", work->saved_id, done);
		break;
	case CHANGE_TEST:
		axis_web_storage_audit(request, "download client %" G_GINT64_FORMAT " tested", work->id);
		done = "tested";
		if (work->back == NULL) {
			location = g_strdup_printf("/downloads/clients/%" G_GINT64_FORMAT, work->id);
			place = "client-state";
			break;
		}
		location = axis_web_setup_return_location(work->back, "tested");
		break;
	case CHANGE_ADD_MAP:
	case CHANGE_REMOVE_MAP:
		axis_web_storage_audit(request, "download client %" G_GINT64_FORMAT ": path mapping %s", work->id,
			work->change == CHANGE_ADD_MAP ? "added" : "removed");
		done = work->change == CHANGE_ADD_MAP ? "map-added" : "map-removed";
		if (work->back == NULL) {
			location = g_strdup_printf("/downloads/clients/%" G_GINT64_FORMAT, work->id);
			place = "client-maps";
			break;
		}
		location = axis_web_setup_return_location(work->back, done);
		break;
	default:
		axis_web_storage_audit(request, "download client %" G_GINT64_FORMAT " removed", work->id);
		location = g_strdup("/downloads/clients?done=deleted");
		break;
	}
	if (place != NULL)
		return axis_web_place_redirect(request, location, done, place);
	return axis_web_auth_redirect(request, location);
}

static HtmxResponse *
start_change(
	AxisWebDownloads *self,
	HtmxRequest      *request,
	ChangeWork       *work
){
	return axis_web_downloads_dispatch(self, request, change_run, change_done, work, change_work_free);
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
		return axis_web_error_respond(request, 404, "Not found", "There is no such download client.");
	}
	values_from_form(&work->values, request);
	if (secret != NULL && secret[0] != '\0' && strlen(secret) <= 4096)
		work->secret = g_strdup(secret);
	work->back = axis_web_setup_request_return(request);
	return start_change(user_data, request, work);
}

static HtmxResponse *
handle_test(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	ChangeWork *work;
	gint64 id = axis_web_storage_param_id(params);

	if (id <= 0)
		return axis_web_error_respond(request, 404, "Not found", "There is no such download client.");
	work = g_new0(ChangeWork, 1);
	work->change = CHANGE_TEST;
	work->id = id;
	work->back = axis_web_setup_request_return(request);
	return start_change(user_data, request, work);
}

static HtmxResponse *
handle_add_map(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	ChangeWork *work;
	gint64 id = axis_web_storage_param_id(params);
	guint64 volume = 0;

	if (id <= 0)
		return axis_web_error_respond(request, 404, "Not found", "There is no such download client.");
	work = g_new0(ChangeWork, 1);
	work->change = CHANGE_ADD_MAP;
	work->id = id;
	work->prefix = form_text(request, "prefix", 2048);
	work->rel_path = form_text(request, "rel_path", 2048);
	if (axis_web_storage_form_u64(request, "volume", &volume) && volume <= G_MAXINT64)
		work->volume_id = (gint64)volume;
	work->back = axis_web_setup_request_return(request);
	return start_change(user_data, request, work);
}

static HtmxResponse *
handle_remove_map(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	ChangeWork *work;
	gint64 id = axis_web_storage_param_id(params);

	if (id <= 0)
		return axis_web_error_respond(request, 404, "Not found", "There is no such download client.");
	work = g_new0(ChangeWork, 1);
	work->change = CHANGE_REMOVE_MAP;
	work->id = id;
	work->prefix = form_text(request, "prefix", 2048);
	return start_change(user_data, request, work);
}

static HtmxResponse *
handle_delete(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	ChangeWork *work;
	gint64 id = axis_web_storage_param_id(params);

	if (id <= 0)
		return axis_web_error_respond(request, 404, "Not found", "There is no such download client.");
	if (!axis_web_storage_form_yes(request, "confirm"))
		return axis_web_error_respond(request, 400, "Not confirmed", "Tick the box to remove the download client.");
	work = g_new0(ChangeWork, 1);
	work->change = CHANGE_DELETE;
	work->id = id;
	return start_change(user_data, request, work);
}

void
axis_web_downloads_register_clients(
	AxisWebDownloads *self,
	AxisWeb          *web
){
	axis_web_route_full(web, HTMX_METHOD_GET, "/downloads/clients", AXIS_WEB_ROUTE_ADMIN, handle_list, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/downloads/clients", AXIS_WEB_ROUTE_ADMIN, handle_save, self);
	axis_web_route_full(web, HTMX_METHOD_GET, "/downloads/clients/:id", AXIS_WEB_ROUTE_ADMIN, handle_client, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/downloads/clients/:id", AXIS_WEB_ROUTE_ADMIN, handle_save, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/downloads/clients/:id/test", AXIS_WEB_ROUTE_ADMIN, handle_test,
		self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/downloads/clients/:id/maps", AXIS_WEB_ROUTE_ADMIN, handle_add_map,
		self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/downloads/clients/:id/maps/remove", AXIS_WEB_ROUTE_ADMIN,
		handle_remove_map, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/downloads/clients/:id/delete", AXIS_WEB_ROUTE_ADMIN, handle_delete,
		self);
}
