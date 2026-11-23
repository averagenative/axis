/*
 * axis-web-imports-assign.c - "Assign by hand" for an import: an
 * administrator says which video file of a download is the movie, or which
 * episodes each file holds (add-force-grab-and-assign, slice B)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 *   GET  /imports/:id/assign             the form: every video file of the
 *                                        import, and a choice for each
 *   POST /imports/:id/assign step=review the confirmation: what each chosen
 *                                        file becomes and is named, what it
 *                                        replaces, its runtime; writes
 *                                        nothing
 *   POST /imports/:id/assign step=apply  (digest=, the ticks) records the
 *                                        assignment and queues the import
 *
 * A choice is posted as one field for each file: to_<position> holds a
 * series' file's episodes as a person writes them ("S01E03", or "S01E03
 * S01E04"); file=<position> names the one file that is the movie.  The ticks
 * are replace_<library file id>=yes and runtime_<position>=yes.  The
 * confirmation holds the choices again as hidden fields with its digest: a
 * post made from a confirmation that is out of date, or without a tick it
 * needs, changes nothing and shows the confirmation again with status 409.
 * All three routes are an administrator's; file names are set as text.
 */

#include "web/axis-web-imports-private.h"

#include <string.h>

#include "core/axis-size.h"
#include "library/axis-library-name.h"
#include "web/axis-web-auth-private.h"
#include "web/axis-web-browser-private.h"
#include "web/axis-web-storage-private.h"
#include "web/axis-web-table.h"

#define ADD axis_web_storage_add

#define TO_PREFIX       "to_"
#define REPLACE_PREFIX  "replace_"
#define RUNTIME_PREFIX  "runtime_"
/* At most this many fields of a kind are read from one post. */
#define ASSIGN_MAX      400

typedef struct
{
	gint64                id;
	AxisImportAssignAsk  *ask;	/* (nullable) the form as it opens */
	gboolean              apply;
	gchar                *digest;
	gchar                *user;
	gboolean              applied;
	gboolean              changed;
	AxisImportAssignment *assignment;
	GError               *error;
} AssignWork;

static void
assign_work_free(gpointer data)
{
	AssignWork *work = data;

	axis_import_assign_ask_free(work->ask);
	g_free(work->digest);
	g_free(work->user);
	axis_import_assignment_free(work->assignment);
	g_clear_error(&work->error);
	g_free(work);
}

static void
assign_run(
	AxisWebImports *self,
	gpointer        data
){
	AssignWork *work = data;

	if (work->apply)
		axis_import_assign(self->import, work->id, work->ask, work->digest, work->user, &work->applied, &work->changed,
			&work->assignment, &work->error);
	else
		axis_import_assign_preview(self->import, work->id, work->ask, &work->assignment, &work->error);
}

static gchar *
minutes_text(gint64 ms)
{
	gint64 minutes = (ms + 30000) / 60000;

	if (ms <= 0)
		return g_strdup("not read");
	if (ms < 60000)
		return g_strdup_printf("%" G_GINT64_FORMAT " s", (ms + 500) / 1000);
	if (minutes >= 60)
		return g_strdup_printf("%" G_GINT64_FORMAT " h %02" G_GINT64_FORMAT " min", minutes / 60, minutes % 60);
	return g_strdup_printf("%" G_GINT64_FORMAT " min", minutes);
}

/* A checkbox that posts "yes", ticked when @checked. */
static HtmxNode *
tick_box(
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

/* "This file is the movie": one of the import's files at most. */
static HtmxNode *
movie_choice(const AxisImportAssignFile *file)
{
	g_autofree gchar *id = g_strdup_printf("assign-file-%d", file->position);
	g_autofree gchar *value = g_strdup_printf("%d", file->position);
	HtmxDiv *field = htmx_div_new();
	HtmxInput *radio = htmx_input_new(HTMX_INPUT_RADIO);
	HtmxLabel *text = htmx_label_new_for(id);

	htmx_element_add_class(HTMX_ELEMENT(field), "field-check");
	htmx_element_set_id(HTMX_ELEMENT(radio), id);
	htmx_input_set_name(radio, "file");
	htmx_input_set_value(radio, value);
	if (file->chosen)
		htmx_element_set_attribute(HTMX_ELEMENT(radio), "checked", "checked");
	htmx_node_set_text_content(HTMX_NODE(text), "This file is the movie");
	ADD(field, radio);
	ADD(field, text);
	return HTMX_NODE(field);
}

static HtmxNode *
file_cell(const AxisImportAssignFile *file)
{
	HtmxTd *cell = htmx_td_new();

	ADD(cell, axis_web_browser_name_node(file->path));
	if (file->read != NULL) {
		g_autofree gchar *says = g_strdup_printf("Axis reads its name as %s.", file->read);

		ADD(cell, axis_web_storage_text("range-detail", says));
	}
	if (file->imported)
		ADD(cell, axis_web_storage_text("range-detail", "In the library already, from this import."));
	if (file->chosen && file->problem != NULL) {
		HtmxNode *problem = axis_web_storage_text("field-error", file->problem);

		htmx_element_set_attribute(HTMX_ELEMENT(problem), "role", "alert");
		ADD(cell, problem);
	}
	return HTMX_NODE(cell);
}

/* The form: every video file, and what the person says it is. */
static HtmxNode *
choose_card(
	HtmxRequest                *request,
	const AxisImportAssignment *a
){
	static const AxisWebColumn columns[] = {
		{ "File", AXIS_WEB_COL_WIDE },
		{ "Size", AXIS_WEB_COL_SIZE },
		{ "Runtime", AXIS_WEB_COL_NUMBER },
		{ "What it is", AXIS_WEB_COL_CONTROLS },
		{ NULL, 0 }
	};
	g_autofree gchar *action = g_strdup_printf("/imports/%" G_GINT64_FORMAT "/assign", a->import_id);
	HtmxNode *card = axis_web_storage_card(a->by_file ? "Say which episodes each file holds" :
		"Say which file is the movie");
	HtmxNode *form = axis_web_auth_post_form(request, action);
	HtmxNode *table = axis_web_table_new("import-assign", columns);
	HtmxTbody *body = htmx_tbody_new();
	guint i;

	ADD(card, axis_web_storage_text("help", a->by_file ?
		"Write the episodes a file holds as S01E03, or S01E03 S01E04 for a file that holds two, in the numbering the "
		"series follows now. Leave a file's field empty to leave that file alone. Axis does not judge the names of the "
		"files you assign, whether their episodes are wanted, or the quality profile." :
		"Choose the one file that is the movie. The other files are left alone. Axis does not judge the name of the "
		"file you choose, or the quality profile."));
	for (i = 0; i < a->files->len; i++) {
		const AxisImportAssignFile *file = g_ptr_array_index(a->files, i);
		g_autofree gchar *size = axis_size_format((guint64)MAX(file->size, 0));
		g_autofree gchar *runs = minutes_text(file->duration_ms);
		HtmxTr *row = htmx_tr_new();
		HtmxTd *what = htmx_td_new();

		ADD(row, file_cell(file));
		ADD(row, htmx_td_new_with_text(size));
		ADD(row, htmx_td_new_with_text(runs));
		if (file->imported) {
			htmx_node_set_text_content(HTMX_NODE(what), "Imported");
		} else if (a->by_file) {
			g_autofree gchar *id = g_strdup_printf("assign-to-%d", file->position);
			g_autofree gchar *name = g_strdup_printf(TO_PREFIX "%d", file->position);

			/* What Axis read from the name is what a person starts from. */
			ADD(what, axis_web_storage_text_field(id, "Its episodes", name, file->chosen ? file->said : file->read,
				"S01E01", NULL));
		} else {
			ADD(what, movie_choice(file));
		}
		ADD(row, what);
		ADD(body, row);
	}
	ADD(table, body);
	ADD(form, table);
	ADD(form, axis_web_auth_hidden("step", "review"));
	ADD(form, axis_web_auth_button("Review the assignment", "button-primary"));
	ADD(card, form);
	return card;
}

/* What one chosen file becomes, with the ticks it needs. */
static HtmxNode *
chosen_item(const AxisImportAssignFile *file)
{
	HtmxLi *item = htmx_li_new();
	g_autofree gchar *shown = axis_library_name_display(file->path);
	g_autofree gchar *runs = minutes_text(file->duration_ms);
	g_autofree gchar *should = minutes_text(file->expected_ms);
	g_autofree gchar *head = g_strdup_printf("%s is imported as %s, %s.", shown, file->label, file->quality);
	guint i;

	ADD(item, axis_web_breakable(head));
	if (file->name != NULL) {
		g_autofree gchar *named = g_strdup_printf("Its name in the library: %s", file->name);

		ADD(item, axis_web_storage_text("range-detail", named));
	}
	if (file->runtime_off) {
		g_autofree gchar *id = g_strdup_printf("assign-runtime-%d", file->position);
		g_autofree gchar *name = g_strdup_printf(RUNTIME_PREFIX "%d", file->position);
		g_autofree gchar *label = g_strdup_printf("It runs %s; %s runs %s. Assign it all the same.", runs, file->label,
			should);

		ADD(item, tick_box(id, label, name, file->runtime_ok));
	} else if (file->expected_ms > 0 && file->duration_ms > 0) {
		g_autofree gchar *text = g_strdup_printf("It runs %s; %s runs %s.", runs, file->label, should);

		ADD(item, axis_web_storage_text("range-detail", text));
	}
	for (i = 0; i < file->replaces->len; i++) {
		const AxisImportAssignReplace *r = g_ptr_array_index(file->replaces, i);
		g_autofree gchar *id = g_strdup_printf("assign-replace-%" G_GINT64_FORMAT, r->file_id);
		g_autofree gchar *name = g_strdup_printf(REPLACE_PREFIX "%" G_GINT64_FORMAT, r->file_id);
		g_autofree gchar *label = g_strdup_printf("Replace %s (%s%s) with this file, which is %s. The replaced file "
			"goes to the recycle folder.", r->path, r->quality, r->kept ? ", kept by hand" : "", r->compare > 0 ?
			"better" : r->compare < 0 ? "worse" : "of the same quality");

		ADD(item, tick_box(id, label, name, r->ticked));
	}
	return HTMX_NODE(item);
}

/* The confirmation: the plan as hidden fields, the ticks, the digest. */
static HtmxNode *
confirm_card(
	HtmxRequest                *request,
	const AxisImportAssignment *a
){
	g_autofree gchar *action = g_strdup_printf("/imports/%" G_GINT64_FORMAT "/assign", a->import_id);
	g_autofree gchar *back = g_strdup_printf("/imports/%" G_GINT64_FORMAT "/assign", a->import_id);
	HtmxNode *card = axis_web_storage_card("Confirm the assignment");
	HtmxNode *form = axis_web_auth_post_form(request, action);
	HtmxUl *list = htmx_ul_new();
	guint i;

	htmx_element_add_class(HTMX_ELEMENT(list), "release-reasons");
	for (i = 0; i < a->files->len; i++) {
		const AxisImportAssignFile *file = g_ptr_array_index(a->files, i);

		if (!file->chosen)
			continue;
		ADD(list, chosen_item(file));
		if (a->by_file) {
			g_autofree gchar *name = g_strdup_printf(TO_PREFIX "%d", file->position);

			ADD(form, axis_web_auth_hidden(name, file->said));
		} else {
			g_autofree gchar *value = g_strdup_printf("%d", file->position);

			ADD(form, axis_web_auth_hidden("file", value));
		}
	}
	ADD(form, list);
	if (a->n_left > 0) {
		g_autofree gchar *left = g_strdup_printf("%u other video %s left alone. They stay in the download. Nothing of "
			"them is imported or deleted by this.", a->n_left, a->n_left == 1 ? "file is" : "files are");

		ADD(form, axis_web_storage_text("help", left));
	}
	ADD(form, axis_web_storage_text("help", "A file in the library is replaced only when you tick it here. Nothing is "
		"deleted. The name shown leaves out what is only read from the file when it is imported (its sound and HDR). "
		"Subtitles named like a chosen file go with it."));
	ADD(form, axis_web_auth_hidden("step", "apply"));
	ADD(form, axis_web_auth_hidden("digest", a->digest));
	ADD(form, axis_web_auth_button("Assign and import", "button-primary"));
	ADD(card, form);
	ADD(card, axis_web_browser_link(back, "Start again"));
	return card;
}

static HtmxResponse *
assign_done(
	AxisWebImports *self,
	HtmxRequest    *request,
	gpointer        data
){
	AssignWork *work = data;
	const AxisImportAssignment *a = work->assignment;
	g_autoptr(HtmxDiv) content = NULL;
	g_autofree gchar *title = NULL;
	g_autofree gchar *back = g_strdup_printf("/imports/%" G_GINT64_FORMAT, work->id);
	HtmxResponse *response;
	HtmxHeading *h1;
	gboolean confirm;
	guint status = 200;
	guint i;

	if (g_error_matches(work->error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_NOT_FOUND))
		return axis_web_error_respond(request, 404, "Not found", "There is no such import.");
	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	if (work->applied) {
		g_autofree gchar *location = g_strdup_printf("/imports/%" G_GINT64_FORMAT "?done=assign_files", work->id);

		axis_web_storage_audit(request, "import %" G_GINT64_FORMAT " files assigned by hand", work->id);
		return axis_web_auth_redirect(request, location);
	}
	/* Refused outright (not waiting for a person, a file that is no video
	 * of the import): nothing to show but why. */
	if (a->refused != NULL && (work->ask != NULL || work->apply))
		return axis_web_error_respond(request, 409, "Not assigned", a->refused);
	content = htmx_div_new();
	title = g_strdup_printf("Assign by hand: %s", a->title_label != NULL ? a->title_label : "an import");
	h1 = htmx_h1_new();
	htmx_node_set_text_content(HTMX_NODE(h1), title);
	ADD(content, h1);
	ADD(content, axis_web_storage_text(NULL, "For a download Axis could not sort by itself: you say what each video "
		"file is, and Axis imports it as that. Files you do not choose are left alone, in the download."));
	if (a->refused != NULL) {
		ADD(content, axis_web_auth_notice(TRUE, a->refused));
		ADD(content, axis_web_browser_link(back, "Back to the import"));
		return axis_web_page_respond(request, title, HTMX_NODE(content));
	}
	confirm = a->n_chosen > 0;
	for (i = 0; i < a->files->len; i++) {
		const AxisImportAssignFile *file = g_ptr_array_index(a->files, i);

		if (file->chosen && file->problem != NULL)
			confirm = FALSE;
	}
	if (work->apply) {
		/* Nothing was written: say why, and show it as it is now. */
		status = 409;
		ADD(content, axis_web_auth_notice(TRUE, work->changed ? "Nothing was assigned: the import, the library or the "
			"series changed since this page was shown. This is how it stands now: look again, then confirm." :
			a->missing != NULL ? a->missing : "Nothing was assigned: a choice cannot be made as it stands."));
		if (a->missing != NULL && work->changed)
			ADD(content, axis_web_auth_notice(TRUE, a->missing));
	} else if (work->ask != NULL && a->n_chosen == 0) {
		ADD(content, axis_web_auth_notice(TRUE, a->by_file ? "Nothing was chosen: write the episodes of at least one "
			"file." : "Nothing was chosen: choose the file that is the movie."));
	}
	if (confirm)
		ADD(content, confirm_card(request, a));
	else
		ADD(content, choose_card(request, a));
	ADD(content, axis_web_browser_link(back, "Back to the import"));
	response = axis_web_page_respond(request, title, HTMX_NODE(content));
	if (status != 200)
		htmx_response_set_status(response, status);
	return response;
}

static HtmxResponse *
handle_assign(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	AssignWork *work;
	gint64 id = axis_web_storage_param_id(params);

	if (id <= 0)
		return axis_web_error_respond(request, 404, "Not found", "There is no such import.");
	work = g_new0(AssignWork, 1);
	work->id = id;
	return axis_web_imports_dispatch(user_data, request, assign_run, assign_done, work, assign_work_free);
}

/* "<prefix><number>" as the number. */
static gboolean
field_number(
	const gchar *name,
	const gchar *prefix,
	guint64      max,
	guint64     *value
){
	if (name == NULL || !g_str_has_prefix(name, prefix))
		return FALSE;
	return g_ascii_string_to_unsigned(name + strlen(prefix), 10, 0, max, value, NULL);
}

static gboolean
digest_valid(const gchar *digest)
{
	guint i;

	if (digest == NULL || strlen(digest) != 32)
		return FALSE;
	for (i = 0; i < 32; i++) {
		if (!g_ascii_isxdigit(digest[i]) || g_ascii_isupper(digest[i]))
			return FALSE;
	}
	return TRUE;
}

static HtmxResponse *
handle_assign_post(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	AssignWork *work;
	gint64 id = axis_web_storage_param_id(params);
	const gchar *step = htmx_request_get_form_value(request, "step");
	const gchar *digest = htmx_request_get_form_value(request, "digest");
	const gchar *file = htmx_request_get_form_value(request, "file");
	GHashTable *fields = htmx_request_get_form_data(request);
	gboolean apply = g_strcmp0(step, "apply") == 0;
	GHashTableIter iter;
	gpointer key;
	gpointer value;
	guint64 number = 0;

	if (id <= 0)
		return axis_web_error_respond(request, 404, "Not found", "There is no such import.");
	if ((!apply && g_strcmp0(step, "review") != 0) || (apply && !digest_valid(digest)))
		return axis_web_error_respond(request, 400, "Not assigned", "Review the assignment first.");
	work = g_new0(AssignWork, 1);
	work->id = id;
	work->apply = apply;
	work->digest = g_strdup(digest);
	work->user = g_strdup(axis_web_request_get_username(request));
	work->ask = axis_import_assign_ask_new();
	/* A movie's one file. */
	if (file != NULL && file[0] != '\0') {
		gint position;

		if (!g_ascii_string_to_unsigned(file, 10, 0, 100000, &number, NULL)) {
			assign_work_free(work);
			return axis_web_error_respond(request, 400, "Not assigned", "Choose a file.");
		}
		position = (gint)number;
		g_array_append_val(work->ask->positions, position);
		g_ptr_array_add(work->ask->said, g_strdup(""));
	}
	if (fields != NULL) {
		g_hash_table_iter_init(&iter, fields);
		while (g_hash_table_iter_next(&iter, &key, &value)) {
			g_autofree gchar *text = value != NULL ? g_strstrip(g_strdup(value)) : NULL;

			if (text == NULL || text[0] == '\0')
				continue;
			if (field_number(key, TO_PREFIX, 100000, &number)) {
				gint position = (gint)number;

				if (work->ask->positions->len >= ASSIGN_MAX || strlen(text) > 400) {
					assign_work_free(work);
					return axis_web_error_respond(request, 400, "Not assigned", "That is too much to assign at once.");
				}
				g_array_append_val(work->ask->positions, position);
				g_ptr_array_add(work->ask->said, g_steal_pointer(&text));
			} else if (field_number(key, RUNTIME_PREFIX, 100000, &number)) {
				gint position = (gint)number;

				if (strcmp(text, "yes") == 0 && work->ask->runtime_ok->len < ASSIGN_MAX)
					g_array_append_val(work->ask->runtime_ok, position);
			} else if (field_number(key, REPLACE_PREFIX, G_MAXINT64, &number)) {
				gint64 file_id = (gint64)number;

				if (strcmp(text, "yes") == 0 && work->ask->replace->len < ASSIGN_MAX)
					g_array_append_val(work->ask->replace, file_id);
			}
		}
	}
	return axis_web_imports_dispatch(user_data, request, assign_run, assign_done, work, assign_work_free);
}

void
axis_web_imports_register_assign(
	AxisWebImports *self,
	AxisWeb        *web
){
	axis_web_route_full(web, HTMX_METHOD_GET, "/imports/:id/assign", AXIS_WEB_ROUTE_ADMIN, handle_assign, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/imports/:id/assign", AXIS_WEB_ROUTE_ADMIN, handle_assign_post, self);
}
