/*
 * axis-web-workers-attach.c - "Attach a worker": a guide of five plain
 * pages on the Conversion workers pages
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 *   1  GET  /workers/attach           the machine: the worker's name, its
 *      POST /workers/attach           container tool, its encoder, its
 *                                     processor (information), and the
 *                                     address it reaches Axis at
 *   2  POST /workers/attach/disks     the disks it reaches, and how each is
 *                                     mounted there
 *   3  POST /workers/attach/mounts    the commands that mount them, to adapt
 *   4  POST /workers/attach/create    the worker is created: its token,
 *                                     shown once, and the text that starts it
 *   5  GET  /workers/attach/waiting   what the worker reported, live
 *      GET  /workers/attach/status    the part of that page that changes
 *
 * A step is a form that posts everything chosen so far, in hidden fields, to
 * the next step's route, and a second form that posts it to the step before:
 * nothing is kept on the server between the steps, and every step can be
 * gone back to.  Each route checks every step before its own again, from
 * the form, and shows the first step whose input is not good.
 *
 * The token exists from step 4 on and nowhere before: steps 1 to 3 create
 * nothing, and the forms and addresses of every step carry the choices only.
 * The page of step 4 is the one answer that holds it, as the page of "Add a
 * worker without the guide" does; the address of step 5 names the worker,
 * its encoder and its disks' numbers.
 *
 * Every text a person is to run or save comes from web/axis-worker-attach.c,
 * made from a plan that passed its check; what was typed is otherwise only
 * shown back in the form it was typed in.
 *
 * Administrators only; every POST has passed the CSRF check before its
 * handler runs (axis_web_route_full()).
 */

#include <string.h>

#include "axis-version.h"
#include "api/axis-worker-proto.h"
#include "api/axis-worker-store.h"
#include "transcode/axis-convert.h"
#include "web/axis-password.h"
#include "web/axis-web-auth-private.h"
#include "web/axis-web-browser-private.h"
#include "web/axis-web-hub.h"
#include "web/axis-web-storage-private.h"
#include "web/axis-web-table.h"
#include "web/axis-web-workers-private.h"
#include "web/axis-worker-attach.h"

#define ADD axis_web_storage_add

/* The longest value a field of the guide may have; a longer one counts as
 * not given. */
#define FIELD_MAX      512

/* How long after a worker was created the last page only waits, before it
 * says what to look at. */
#define PATIENCE_US    (G_GINT64_CONSTANT(120) * G_USEC_PER_SEC)

/* How often the last page asks again by itself, until everything is good. */
#define POLL_TRIGGER   "every 5s"

typedef enum
{
	STEP_NONE,
	STEP_MACHINE,
	STEP_DISKS,
	STEP_MOUNTS,
	STEP_CREATE
} Step;

/* ── What Axis knows: the disks ─────────────────────────────────── */

typedef struct
{
	gint64    id;
	gchar    *name;
	gchar    *uuid;
	gboolean  marked;	/* it has a marker: a worker can prove it */
} Disk;

static void
disk_free(gpointer data)
{
	Disk *disk = data;

	g_free(disk->name);
	g_free(disk->uuid);
	g_free(disk);
}

static GPtrArray *
disks_load(
	AxisDbPool  *pool,
	GError     **error
){
	g_autoptr(AxisDbResult) result = NULL;
	GPtrArray *disks;
	guint i;

	if (!axis_db_pool_exec(pool, "SELECT id, name, coalesce(uuid::text, ''), identity <> 'filesystem' FROM volume"
	        " ORDER BY name, id", 0, NULL, &result, error))
		return NULL;
	disks = g_ptr_array_new_with_free_func(disk_free);
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		Disk *disk = g_new0(Disk, 1);

		axis_db_result_get_int64(result, i, 0, &disk->id);
		disk->name = g_strdup(axis_db_result_get_text(result, i, 1));
		disk->uuid = g_strdup(axis_db_result_get_text(result, i, 2));
		axis_db_result_get_boolean(result, i, 3, &disk->marked);
		g_ptr_array_add(disks, disk);
	}
	return disks;
}

static const Disk *
disk_find(
	GPtrArray *disks,
	gint64     id
){
	guint i;

	for (i = 0; disks != NULL && i < disks->len; i++) {
		const Disk *disk = g_ptr_array_index(disks, i);

		if (disk->id == id)
			return disk;
	}
	return NULL;
}

/* ── What was chosen so far: the form ───────────────────────────── */

typedef struct
{
	gint64  volume;
	gchar  *kind;	/* as posted */
	gchar  *host;	/* as typed */
} Ticked;

typedef struct
{
	gchar     *name;	/* every one as posted, or NULL */
	gchar     *tool;
	gchar     *encoder;
	gchar     *arch;
	gchar     *url;
	gboolean   allow_http;
	GPtrArray *ticked;	/* Ticked, by the disk's number */
} Form;

static void
ticked_free(gpointer data)
{
	Ticked *ticked = data;

	g_free(ticked->kind);
	g_free(ticked->host);
	g_free(ticked);
}

static void
form_free(Form *form)
{
	if (form == NULL)
		return;
	g_free(form->name);
	g_free(form->tool);
	g_free(form->encoder);
	g_free(form->arch);
	g_free(form->url);
	g_ptr_array_unref(form->ticked);
	g_free(form);
}

/* A field as posted, when it is text of a bounded length. */
static gchar *
field(
	HtmxRequest *request,
	const gchar *name
){
	const gchar *value = htmx_request_get_form_value(request, name);

	if (value == NULL || strlen(value) > FIELD_MAX || !g_utf8_validate(value, -1, NULL))
		return NULL;
	return g_strdup(value);
}

static gint
ticked_compare(
	gconstpointer a,
	gconstpointer b
){
	const Ticked *x = *(const Ticked *const *)a;
	const Ticked *y = *(const Ticked *const *)b;

	return x->volume < y->volume ? -1 : x->volume > y->volume ? 1 : 0;
}

/* The number in "disk-N": 1 to 18 digits, no sign, no leading zero. */
static gboolean
disk_field_number(
	const gchar *key,
	gint64      *number
){
	const gchar *digits;
	const gchar *p;

	if (!g_str_has_prefix(key, "disk-"))
		return FALSE;
	digits = key + 5;
	if (digits[0] == '\0' || digits[0] == '0' || strlen(digits) > 18)
		return FALSE;
	for (p = digits; *p != '\0'; p++) {
		if (!g_ascii_isdigit(*p))
			return FALSE;
	}
	*number = g_ascii_strtoll(digits, NULL, 10);
	return TRUE;
}

static Form *
form_read(HtmxRequest *request)
{
	Form *form = g_new0(Form, 1);
	GHashTable *posted = htmx_request_get_form_data(request);
	GHashTableIter iter;
	gpointer key;
	gpointer value;

	form->ticked = g_ptr_array_new_with_free_func(ticked_free);
	form->name = field(request, "name");
	form->tool = field(request, "tool");
	form->encoder = field(request, "encoder");
	form->arch = field(request, "arch");
	form->url = field(request, "url");
	form->allow_http = axis_web_storage_form_yes(request, "allow_http");
	if (posted == NULL)
		return form;
	g_hash_table_iter_init(&iter, posted);
	while (g_hash_table_iter_next(&iter, &key, &value)) {
		gint64 number = 0;
		Ticked *ticked;

		if (value == NULL || strcmp(value, "yes") != 0 || !disk_field_number(key, &number))
			continue;
		/* One more than a plan takes is enough to say there are too many. */
		if (form->ticked->len > AXIS_ATTACH_MAX_DISKS)
			break;
		{
			g_autofree gchar *kind = g_strdup_printf("kind-%" G_GINT64_FORMAT, number);
			g_autofree gchar *host = g_strdup_printf("host-%" G_GINT64_FORMAT, number);

			ticked = g_new0(Ticked, 1);
			ticked->volume = number;
			ticked->kind = field(request, kind);
			ticked->host = field(request, host);
			g_ptr_array_add(form->ticked, ticked);
		}
	}
	g_ptr_array_sort(form->ticked, ticked_compare);
	return form;
}

static const Ticked *
form_ticked(
	const Form *form,
	gint64      volume
){
	guint i;

	for (i = 0; form != NULL && i < form->ticked->len; i++) {
		const Ticked *ticked = g_ptr_array_index(form->ticked, i);

		if (ticked->volume == volume)
			return ticked;
	}
	return NULL;
}

/* ── One request ────────────────────────────────────────────────── */

typedef struct
{
	Step            target;		/* the step whose page was asked for */
	Form           *form;		/* NULL: the first page, nothing chosen yet */
	gchar          *offered_url;	/* the address the browser used, in its plain form, or NULL */
	gchar          *by;

	GPtrArray      *disks;
	AxisAttachPlan *plan;		/* as far as it was checked */
	Step            wrong;		/* the first step whose input is not good, or STEP_NONE */
	gchar          *problem;	/* a sentence for that step's page */
	gchar          *token;		/* secret: step 4 only */
	GError         *error;
} AttachWork;

static void
attach_work_free(gpointer data)
{
	AttachWork *work = data;

	form_free(work->form);
	g_free(work->offered_url);
	g_free(work->by);
	g_clear_pointer(&work->disks, g_ptr_array_unref);
	g_clear_pointer(&work->plan, axis_attach_plan_free);
	g_free(work->problem);
	axis_secret_free(work->token);
	g_clear_error(&work->error);
	g_free(work);
}

static void
wrong(
	AttachWork  *work,
	Step         step,
	const gchar *sentence
){
	work->wrong = step;
	g_free(work->problem);
	work->problem = g_strdup(sentence);
}

/* Step 1 of the form into the plan.  FALSE: work->wrong says why. */
static gboolean
check_machine(
	AxisWebWorkers *self,
	AttachWork     *work
){
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisDbResult) taken = NULL;
	const Form *form = work->form;
	AxisAttachPlan *plan = work->plan;
	gboolean plain_elsewhere = FALSE;
	const gchar *params[1];

	{
		g_autofree gchar *typed = g_strstrip(g_strdup(form->name != NULL ? form->name : ""));

		plan->name = g_ascii_strdown(typed, -1);
	}
	if (!axis_worker_name_is_valid(plan->name)) {
		wrong(work, STEP_MACHINE, "A worker's name is 1 to 48 lower-case letters, digits and '-', starting with a "
			"letter.");
		return FALSE;
	}
	if (!axis_attach_tool_from_word(form->tool, &plan->tool) ||
	    !axis_attach_encoder_from_word(form->encoder, &plan->encoder) ||
	    !axis_attach_arch_from_word(form->arch, &plan->arch)) {
		wrong(work, STEP_MACHINE, "Choose the container tool, the encoder and the processor from the lists.");
		return FALSE;
	}
	if (!axis_attach_url_normalize(form->url, &plan->url, &plain_elsewhere, &error)) {
		wrong(work, STEP_MACHINE, error->message);
		return FALSE;
	}
	plan->allow_http = form->allow_http && plain_elsewhere;
	if (plain_elsewhere && !plan->allow_http) {
		wrong(work, STEP_MACHINE, "The address is plain http to another machine, and a worker refuses to send its "
			"token that way. Use an https address (Axis behind a reverse proxy with TLS), or tick the box that says "
			"the network between the two is trusted.");
		return FALSE;
	}
	params[0] = plan->name;
	if (!axis_db_pool_exec(self->pool, "SELECT 1 FROM worker WHERE name = $1::text", 1, params, &taken, &work->error))
		return FALSE;
	if (axis_db_result_get_n_rows(taken) > 0) {
		wrong(work, STEP_MACHINE, "There is a worker of that name already (a revoked worker keeps its name). Choose "
			"another name.");
		return FALSE;
	}
	return TRUE;
}

/* Step 2 of the form into the plan. */
static gboolean
check_disks(AttachWork *work)
{
	g_autoptr(GError) error = NULL;
	const Form *form = work->form;
	guint i;

	for (i = 0; i < form->ticked->len; i++) {
		const Ticked *ticked = g_ptr_array_index(form->ticked, i);
		const Disk *disk = disk_find(work->disks, ticked->volume);
		g_autofree gchar *host = NULL;
		AxisAttachMount mount = AXIS_ATTACH_MOUNT_BIND;

		if (disk == NULL) {
			wrong(work, STEP_DISKS, "One of the ticked disks is not a disk Axis knows any more. Choose again.");
			return FALSE;
		}
		if (!disk->marked) {
			wrong(work, STEP_DISKS, "A ticked disk has no marker file yet, so no worker can recognise it. Untick it; "
				"Axis writes the marker the first time it finds the disk mounted read-write.");
			return FALSE;
		}
		if (!axis_attach_mount_from_word(ticked->kind, &mount)) {
			wrong(work, STEP_DISKS, "Choose how each ticked disk is mounted from the list.");
			return FALSE;
		}
		if (!axis_attach_host_path_normalize(ticked->host, &host, &error)) {
			wrong(work, STEP_DISKS, error->message);
			return FALSE;
		}
		axis_attach_plan_add_disk(work->plan, disk->id, host, mount);
	}
	if (!axis_attach_plan_check(work->plan, &error)) {
		wrong(work, STEP_DISKS, error->message);
		return FALSE;
	}
	return TRUE;
}

static void
attach_run(
	AxisWebWorkers *self,
	gpointer        data
){
	AttachWork *work = data;
	gint64 id = 0;

	work->disks = disks_load(self->pool, &work->error);
	if (work->disks == NULL || work->target == STEP_MACHINE || work->form == NULL)
		return;
	work->plan = axis_attach_plan_new();
	if (!check_machine(self, work) || work->target == STEP_DISKS)
		return;
	if (!check_disks(work) || work->target == STEP_MOUNTS)
		return;

	/* Step 4, and only here: the worker and its token. */
	if (!axis_worker_store_create(self->pool, self->tokens, work->plan->name, work->by, &work->token, &id,
	        &work->error)) {
		if (g_error_matches(work->error, AXIS_WORKER_STORE_ERROR, AXIS_WORKER_STORE_ERROR_EXISTS))
			wrong(work, STEP_MACHINE, "There is a worker of that name already (a revoked worker keeps its name). "
				"Choose another name.");
		else if (g_error_matches(work->error, AXIS_WORKER_STORE_ERROR, AXIS_WORKER_STORE_ERROR_INVALID))
			wrong(work, STEP_MACHINE, "A worker's name is 1 to 48 lower-case letters, digits and '-', starting with "
				"a letter.");
		if (work->wrong != STEP_NONE)
			g_clear_error(&work->error);
	}
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

/* Text to select and copy: a block, nothing else. */
static HtmxNode *
block(
	const gchar *text,
	const gchar *css_class
){
	HtmxPre *pre = htmx_pre_new();

	if (css_class != NULL)
		htmx_element_add_class(HTMX_ELEMENT(pre), css_class);
	ADD(pre, htmx_code_new_with_text(text != NULL ? text : ""));
	return HTMX_NODE(pre);
}

typedef const gchar *(*WordFunc)(guint index);

static const gchar *tool_word(guint i) { return axis_attach_tool_word((AxisAttachTool)i); }
static const gchar *tool_label(guint i) { return axis_attach_tool_label((AxisAttachTool)i); }
static const gchar *encoder_word(guint i) { return axis_attach_encoder_word((AxisAttachEncoder)i); }
static const gchar *encoder_label(guint i) { return axis_attach_encoder_label((AxisAttachEncoder)i); }
static const gchar *arch_word(guint i) { return axis_attach_arch_word((AxisAttachArch)i); }
static const gchar *arch_label(guint i) { return axis_attach_arch_label((AxisAttachArch)i); }
static const gchar *mount_word(guint i) { return axis_attach_mount_word((AxisAttachMount)i); }
static const gchar *mount_label(guint i) { return axis_attach_mount_label((AxisAttachMount)i); }

/* A labelled list of fixed choices; the one whose word is @current is
 * chosen.  The values are the fixed words, never anything typed. */
static HtmxNode *
choice_field(
	const gchar *id,
	const gchar *label,
	const gchar *name,
	guint        n,
	WordFunc     word,
	WordFunc     text,
	const gchar *current,
	const gchar *help
){
	HtmxDiv *box = htmx_div_new();
	HtmxLabel *caption = htmx_label_new_for(id);
	HtmxSelect *select = htmx_select_new_with_name(name);
	guint i;

	htmx_element_add_class(HTMX_ELEMENT(box), "field");
	htmx_node_set_text_content(HTMX_NODE(caption), label);
	htmx_element_set_id(HTMX_ELEMENT(select), id);
	for (i = 0; i < n; i++) {
		HtmxOption *option = htmx_option_new_with_value(word(i), text(i));

		if (g_strcmp0(current, word(i)) == 0)
			htmx_option_set_selected(option, TRUE);
		ADD(select, option);
	}
	ADD(box, caption);
	ADD(box, select);
	if (help != NULL)
		ADD(box, axis_web_storage_text("help", help));
	return HTMX_NODE(box);
}

static HtmxNode *
check_field(
	const gchar *id,
	const gchar *label,
	const gchar *name,
	gboolean     checked
){
	HtmxDiv *box = htmx_div_new();
	HtmxInput *input = htmx_input_new(HTMX_INPUT_CHECKBOX);
	HtmxLabel *caption = htmx_label_new_for(id);

	htmx_element_add_class(HTMX_ELEMENT(box), "field-check");
	htmx_element_set_id(HTMX_ELEMENT(input), id);
	htmx_input_set_name(input, name);
	htmx_input_set_value(input, "yes");
	if (checked)
		htmx_element_set_attribute(HTMX_ELEMENT(input), "checked", "checked");
	htmx_node_set_text_content(HTMX_NODE(caption), label);
	ADD(box, input);
	ADD(box, caption);
	return HTMX_NODE(box);
}

/* Step 1's choices, carried as they were posted. */
static void
carry_machine(
	HtmxNode   *form_node,
	const Form *form
){
	if (form == NULL)
		return;
	ADD(form_node, axis_web_auth_hidden("name", form->name));
	ADD(form_node, axis_web_auth_hidden("tool", form->tool));
	ADD(form_node, axis_web_auth_hidden("encoder", form->encoder));
	ADD(form_node, axis_web_auth_hidden("arch", form->arch));
	ADD(form_node, axis_web_auth_hidden("url", form->url));
	if (form->allow_http)
		ADD(form_node, axis_web_auth_hidden("allow_http", "yes"));
}

/* Step 2's choices, carried as they were posted.  The fields' names are
 * made from the disks' numbers. */
static void
carry_disks(
	HtmxNode   *form_node,
	const Form *form
){
	guint i;

	for (i = 0; form != NULL && i < form->ticked->len && i < AXIS_ATTACH_MAX_DISKS; i++) {
		const Ticked *ticked = g_ptr_array_index(form->ticked, i);
		g_autofree gchar *disk = g_strdup_printf("disk-%" G_GINT64_FORMAT, ticked->volume);
		g_autofree gchar *kind = g_strdup_printf("kind-%" G_GINT64_FORMAT, ticked->volume);
		g_autofree gchar *host = g_strdup_printf("host-%" G_GINT64_FORMAT, ticked->volume);

		ADD(form_node, axis_web_auth_hidden(disk, "yes"));
		ADD(form_node, axis_web_auth_hidden(kind, ticked->kind));
		ADD(form_node, axis_web_auth_hidden(host, ticked->host));
	}
}

/* The form that goes to the step before, with everything chosen so far. */
static HtmxNode *
back_form(
	HtmxRequest *request,
	const gchar *action,
	const gchar *label,
	const Form  *form
){
	HtmxNode *node = axis_web_auth_post_form(request, action);

	htmx_element_add_class(HTMX_ELEMENT(node), "inline-form");
	carry_machine(node, form);
	carry_disks(node, form);
	ADD(node, axis_web_auth_button(label, "button-quiet"));
	return node;
}

static HtmxNode *
page_top(
	const gchar *step,
	const gchar *problem
){
	HtmxDiv *content = htmx_div_new();

	ADD(content, heading(1, "Attach a worker"));
	ADD(content, axis_web_page_intro(step));
	if (problem != NULL)
		ADD(content, axis_web_auth_notice(TRUE, problem));
	return HTMX_NODE(content);
}

static HtmxResponse *
respond(
	HtmxRequest *request,
	HtmxNode    *content,	/* taken */
	gboolean     refused
){
	HtmxResponse *response = axis_web_page_respond(request, "Attach a worker", content);

	if (refused)
		htmx_response_set_status(response, 400);
	g_object_unref(content);
	return response;
}

/* ── Step 1: the machine ────────────────────────────────────────── */

static HtmxResponse *
page_machine(
	HtmxRequest *request,
	AttachWork  *work
){
	const Form *form = work->form;
	HtmxNode *content = page_top("Step 1 of 5: the machine. Say what the worker runs on. Nothing is created until "
		"step 4.", work->wrong == STEP_MACHINE ? work->problem : NULL);
	HtmxNode *card = axis_web_storage_card("The machine");
	HtmxNode *node = axis_web_auth_post_form(request, "/workers/attach/disks");

	ADD(node, axis_web_storage_text_field("attach-name", "Name of the worker", "name",
		form != NULL ? form->name : NULL, "desk", "Lower-case letters, digits and '-', starting with a letter. It "
		"names the worker on the pages and its container on that machine."));
	ADD(node, choice_field("attach-tool", "Container tool on that machine", "tool", AXIS_ATTACH_N_TOOLS, tool_word,
		tool_label, form != NULL ? form->tool : "podman", "The text you get in step 4 is for this tool."));
	ADD(node, choice_field("attach-encoder", "Encoder", "encoder", AXIS_ATTACH_N_ENCODERS, encoder_word,
		encoder_label, form != NULL ? form->encoder : "nvenc", "The worker is told to use this family of encoders "
		"only, and is given the GPU when there is one. With a GPU, the software encoders are left out on purpose: "
		"the built-in profiles prefer them when a worker offers them, and they are many times slower."));
	ADD(node, choice_field("attach-arch", "Processor", "arch", AXIS_ATTACH_N_ARCHS, arch_word, arch_label,
		form != NULL ? form->arch : "x86-64", "For your information only: the published image has both, and the "
		"container tool takes the right one by itself."));
	ADD(node, axis_web_storage_text_field("attach-url", "Address the worker reaches Axis at", "url",
		form != NULL ? form->url : work->offered_url, "https://axis.example.org", "As seen from the worker's "
		"machine. The address your browser uses is offered when Axis can tell it; change it if the worker reaches "
		"Axis another way."));
	ADD(node, check_field("attach-allow-http", "The network between the worker and Axis is trusted: allow plain "
		"http to another machine (the token crosses that network unencrypted)", "allow_http",
		form != NULL && form->allow_http));
	carry_disks(node, form);
	ADD(node, axis_web_auth_button("Next: the disks", NULL));
	ADD(card, node);
	ADD(content, card);
	ADD(content, axis_web_browser_link("/workers", "Back to Conversion workers"));
	return respond(request, content, work->wrong != STEP_NONE);
}

/* ── Step 2: the disks ──────────────────────────────────────────── */

static HtmxNode *
disk_fields(
	const Disk   *disk,
	const Ticked *ticked
){
	HtmxFieldset *set = htmx_fieldset_new();
	g_autofree gchar *tick_id = g_strdup_printf("attach-disk-%" G_GINT64_FORMAT, disk->id);
	g_autofree gchar *tick_name = g_strdup_printf("disk-%" G_GINT64_FORMAT, disk->id);
	g_autofree gchar *kind_id = g_strdup_printf("attach-kind-%" G_GINT64_FORMAT, disk->id);
	g_autofree gchar *kind_name = g_strdup_printf("kind-%" G_GINT64_FORMAT, disk->id);
	g_autofree gchar *host_id = g_strdup_printf("attach-host-%" G_GINT64_FORMAT, disk->id);
	g_autofree gchar *host_name = g_strdup_printf("host-%" G_GINT64_FORMAT, disk->id);
	g_autofree gchar *suggested = g_strdup_printf("/mnt/axis/disk-%" G_GINT64_FORMAT, disk->id);
	g_autofree gchar *inside = axis_attach_container_path(disk->id);
	g_autofree gchar *help = g_strdup_printf("Where the top of this disk is, or will be mounted, on the worker's "
		"machine. Letters, digits and / . _ - only. Inside its container the worker sees it at %s.", inside);

	htmx_element_add_class(HTMX_ELEMENT(set), "attach-disk");
	/* The disk's name is text a person gave it: shown as text, never part of
	 * a field's name or of the generated text. */
	ADD(set, htmx_legend_new_with_text(disk->name));
	if (!disk->marked) {
		ADD(set, axis_web_storage_text("help", "This disk has no marker file yet, so no worker can recognise it. "
			"Axis writes the marker the first time it finds the disk mounted read-write; attach the worker to it "
			"afterwards."));
		return HTMX_NODE(set);
	}
	ADD(set, check_field(tick_id, "The worker reaches this disk", tick_name, ticked != NULL));
	ADD(set, choice_field(kind_id, "How it is mounted on that machine", kind_name, AXIS_ATTACH_N_MOUNTS, mount_word,
		mount_label, ticked != NULL ? ticked->kind : "nfs", NULL));
	ADD(set, axis_web_storage_text_field(host_id, "Its path on that machine", host_name,
		ticked != NULL ? ticked->host : suggested, suggested, help));
	return HTMX_NODE(set);
}

static HtmxResponse *
page_disks(
	HtmxRequest *request,
	AttachWork  *work
){
	const Form *form = work->form;
	HtmxNode *content = page_top("Step 2 of 5: the disks. Tick the disks the worker will reach, and say how each is "
		"mounted on its machine.", work->wrong == STEP_DISKS ? work->problem : NULL);
	HtmxNode *card = axis_web_storage_card("The disks Axis knows");
	guint i;

	ADD(card, axis_web_storage_text(NULL, "A worker is given work only on a disk it has proved: it reads the disk's "
		"marker file, .axis-volume, at the top of the disk as it sees it. Mount each disk whole and writable, so "
		"that its top directory (with .axis-volume and the work area .axis-work) is a directory on the worker's "
		"machine."));
	ADD(card, axis_web_storage_text("help", "NFS keeps file identities (inode numbers, times to the nanosecond), so "
		"the worker checks every file as strictly as this server. sshfs and SMB do not: for those disks the worker "
		"is told to check a file by its size, its time to the second and samples of its content. A disk of the "
		"worker's own machine needs no network mount."));
	if (work->disks->len == 0) {
		ADD(card, axis_web_storage_text("notice notice-warning", "Axis knows no disk yet. Add your disks first, then "
			"come back."));
		ADD(card, axis_web_browser_link("/storage", "Disks"));
	} else {
		HtmxNode *node = axis_web_auth_post_form(request, "/workers/attach/mounts");

		carry_machine(node, form);
		for (i = 0; i < work->disks->len; i++) {
			const Disk *disk = g_ptr_array_index(work->disks, i);

			ADD(node, disk_fields(disk, form_ticked(form, disk->id)));
		}
		ADD(node, axis_web_auth_button("Next: mount the disks", NULL));
		ADD(card, node);
	}
	ADD(content, card);
	ADD(content, back_form(request, "/workers/attach", "Back: the machine", form));
	return respond(request, content, work->wrong != STEP_NONE);
}

/* ── Step 3: mount the disks ────────────────────────────────────── */

static HtmxNode *
plan_facts(const AxisAttachPlan *plan)
{
	HtmxNode *facts = axis_web_browser_facts_new();
	g_autofree gchar *n_disks = g_strdup_printf("%u", plan->disks->len);

	axis_web_browser_fact(facts, "Worker", plan->name);
	axis_web_browser_fact(facts, "Container tool", axis_attach_tool_label(plan->tool));
	axis_web_browser_fact(facts, "Encoder", axis_attach_encoder_label(plan->encoder));
	axis_web_browser_fact(facts, "Processor", axis_attach_arch_label(plan->arch));
	axis_web_browser_fact(facts, "Reaches Axis at", plan->url);
	axis_web_browser_fact(facts, "Disks", n_disks);
	return facts;
}

static HtmxResponse *
page_mounts(
	HtmxRequest *request,
	AttachWork  *work
){
	const Form *form = work->form;
	const AxisAttachPlan *plan = work->plan;
	HtmxNode *content = page_top("Step 3 of 5: mount the disks. Do this on the worker's machine before you start "
		"the worker.", NULL);
	HtmxNode *card = axis_web_storage_card("Commands to adapt");
	HtmxNode *next = axis_web_storage_card("Create the worker");
	HtmxNode *node = axis_web_auth_post_form(request, "/workers/attach/create");
	guint i;

	ADD(card, axis_web_storage_text(NULL, "Replace the words in capitals: SERVER is the address of the machine that "
		"holds the disk, /PATH/ON/SERVER the disk's top directory there (SHARE for SMB), and AXIS_USER the user Axis "
		"runs as there. Axis does not know these and does not guess them. The mount must reach the server as "
		"Axis's user, because Axis accepts only converted files that its own user owns."));
	for (i = 0; i < plan->disks->len; i++) {
		const AxisAttachDisk *disk = g_ptr_array_index(plan->disks, i);
		const Disk *known = disk_find(work->disks, disk->volume);
		g_autofree gchar *text = axis_attach_mount_text(disk);

		ADD(card, heading(3, known != NULL ? known->name : "A disk"));
		ADD(card, axis_web_storage_text("help", axis_attach_mount_label(disk->mount)));
		ADD(card, block(text, "attach-text"));
	}
	ADD(card, axis_web_storage_text("help", "Each disk is ready when the last command shows .axis-volume. A mount "
		"made by hand is gone after a restart: put it in /etc/fstab, or a unit of its own, once it works."));
	ADD(content, card);

	ADD(next, plan_facts(plan));
	ADD(next, axis_web_storage_text("notice notice-warning", "The next step creates the worker and shows its token. "
		"The token is shown once: have the worker's machine at hand."));
	carry_machine(node, form);
	carry_disks(node, form);
	ADD(node, axis_web_auth_button("Create the worker and show its token", NULL));
	ADD(next, node);
	ADD(content, next);
	ADD(content, back_form(request, "/workers/attach/disks", "Back: the disks", form));
	return respond(request, content, FALSE);
}

/* ── Step 4: the worker, its token and its text ─────────────────── */

/* The address of step 5 for a plan: the worker's name, its encoder's word
 * and its disks' numbers, all of fixed characters. */
static gchar *
waiting_query(const AxisAttachPlan *plan)
{
	GString *query = g_string_new(NULL);
	guint i;

	g_string_append_printf(query, "name=%s&encoder=%s&disks=", plan->name, axis_attach_encoder_word(plan->encoder));
	for (i = 0; i < plan->disks->len; i++) {
		const AxisAttachDisk *disk = g_ptr_array_index(plan->disks, i);

		g_string_append_printf(query, "%s%" G_GINT64_FORMAT, i > 0 ? "." : "", disk->volume);
	}
	return g_string_free(query, FALSE);
}

static const gchar *
encoder_needs(AxisAttachEncoder encoder)
{
	switch (encoder) {
	case AXIS_ATTACH_ENCODER_NVENC:
		return "The NVIDIA GPU needs, on that machine, the NVIDIA driver (nvidia-smi works) and the NVIDIA Container "
			"Toolkit. With Podman the toolkit describes the GPU as a CDI device: run \"sudo nvidia-ctk cdi generate "
			"--output=/etc/cdi/nvidia.yaml\" once (and again after a driver update); \"nvidia-ctk cdi list\" must "
			"name nvidia.com/gpu=all.";
	case AXIS_ATTACH_ENCODER_VAAPI:
		return "The GPU's render node must be /dev/dri/renderD128 on that machine (look with \"ls -l /dev/dri\"; "
			"change the text if yours has another number), the GPU's VA-API driver must be installed there, and your "
			"user must be in the group that owns the node (usually render or video).";
	default:
		return "The software encoders need nothing on that machine. They are slow: on a small board, a film can "
			"take many hours.";
	}
}

static HtmxResponse *
page_result(
	HtmxRequest *request,
	AttachWork  *work
){
	const AxisAttachPlan *plan = work->plan;
	g_autofree gchar *text = axis_attach_generate(plan, AXIS_VERSION);
	g_autofree gchar *token_text = axis_attach_token_text(plan);
	g_autofree gchar *start_text = axis_attach_start_text(plan);
	g_autofree gchar *file = axis_attach_file_name(plan);
	g_autofree gchar *query = waiting_query(plan);
	g_autofree gchar *waiting = g_strdup_printf("/workers/attach/waiting?%s", query);
	g_autofree gchar *title = g_strdup_printf("Worker %s", plan->name);
	g_autofree gchar *save = NULL;
	HtmxNode *content = page_top("Step 4 of 5: start the worker. Do the three parts below on the worker's machine, "
		"in this order.", NULL);
	HtmxNode *token_card = axis_web_storage_card("1. Put the token in a file");
	HtmxNode *text_card;
	HtmxNode *start_card = axis_web_storage_card("3. Start it");
	HtmxResponse *response;

	if (text == NULL || token_text == NULL || start_text == NULL) {
		/* A plan that passed its check always has text. */
		g_object_unref(content);
		g_object_unref(token_card);
		g_object_unref(start_card);
		return axis_web_internal_error(request, NULL);
	}
	axis_web_storage_audit(request, "worker %s created with the guide: tool=%s encoder=%s disks=%u", plan->name,
		axis_attach_tool_word(plan->tool), axis_attach_encoder_word(plan->encoder), plan->disks->len);
	ADD(content, axis_web_auth_notice(FALSE, "The worker has been added."));

	ADD(token_card, axis_web_storage_text("notice notice-warning", "This is the only time this token is shown. Copy "
		"it now: only a hash of it is kept, so it cannot be shown again. If it is lost, revoke the worker and attach "
		"another."));
	ADD(token_card, block(work->token, "worker-token"));
	ADD(token_card, axis_web_storage_text(NULL, "Put it in a file only your user on that machine can read. Never "
		"put it on a command line or in an environment variable, where every user of that machine can see it. The "
		"text below names the file and never the token."));
	ADD(token_card, block(token_text, "attach-text"));
	ADD(content, token_card);

	if (file != NULL) {
		save = g_strdup_printf("Save this as %s.", file);
		text_card = axis_web_storage_card("2. The file for this worker");
		ADD(text_card, axis_web_storage_text(NULL, save));
	} else {
		text_card = axis_web_storage_card("2. The command for this worker");
		ADD(text_card, axis_web_storage_text(NULL, "Run it as your own user once the token file is there and the "
			"disks are mounted."));
	}
	ADD(text_card, block(text, "attach-text"));
	ADD(text_card, axis_web_storage_text("help", "The container runs read-only, without capabilities, without new "
		"privileges, with a limit on its processes, and writes only to /tmp (a tmpfs) and to the work area of each "
		"disk. The image is this server's version: a worker of another version is refused."));
	ADD(text_card, axis_web_storage_text("help", encoder_needs(plan->encoder)));
	ADD(content, text_card);

	ADD(start_card, block(start_text, "attach-text"));
	ADD(start_card, axis_web_storage_text("help", "The worker's log says \"registered as\" and, for each disk, "
		"\"proved and accepted\" when it works."));
	ADD(content, start_card);

	ADD(content, axis_web_storage_text("notice notice-warning", "Copy the token before you leave this page."));
	ADD(content, axis_web_browser_link(waiting, "Next: wait for the worker"));
	response = axis_web_page_respond(request, title, content);
	g_object_unref(content);
	return response;
}

static HtmxResponse *
attach_done(
	AxisWebWorkers *self,
	HtmxRequest    *request,
	gpointer        data
){
	AttachWork *work = data;
	Step show = work->wrong != STEP_NONE ? work->wrong : work->target;

	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	switch (show) {
	case STEP_DISKS:
		return page_disks(request, work);
	case STEP_MOUNTS:
		return page_mounts(request, work);
	case STEP_CREATE:
		return page_result(request, work);
	default:
		return page_machine(request, work);
	}
}

/* The address the browser used to reach Axis, in its plain form, as the
 * default for the worker's: the scheme and the host of this request (a
 * trusted proxy's, when there is one).  NULL when it has no such form.
 * Nothing else of this host is offered. */
static gchar *
browser_address(HtmxRequest *request)
{
	const AxisAuthState *state = axis_web_auth_state(request);
	SoupServerMessage *message = htmx_request_get_message(request);
	const gchar *host = state != NULL ? state->forwarded_host : NULL;
	g_autofree gchar *typed = NULL;
	gchar *url = NULL;

	if (host == NULL && message != NULL)
		host = soup_message_headers_get_one(soup_server_message_get_request_headers(message), "Host");
	if (host == NULL || strlen(host) > AXIS_ATTACH_URL_MAX)
		return NULL;
	typed = g_strdup_printf("%s://%s", state != NULL && state->https ? "https" : "http", host);
	if (!axis_attach_url_normalize(typed, &url, NULL, NULL))
		return NULL;
	return url;
}

static HtmxResponse *
attach_start(
	AxisWebWorkers *self,
	HtmxRequest    *request,
	Step            target,
	gboolean        posted
){
	AttachWork *work = g_new0(AttachWork, 1);

	work->target = target;
	work->form = posted ? form_read(request) : NULL;
	work->offered_url = posted ? NULL : browser_address(request);
	work->by = g_strdup(axis_web_request_get_username(request));
	return axis_web_workers_dispatch(self, request, attach_run, attach_done, work, attach_work_free);
}

static HtmxResponse *
handle_first(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	return attach_start(user_data, request, STEP_MACHINE, FALSE);
}

static HtmxResponse *
handle_machine(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	return attach_start(user_data, request, STEP_MACHINE, TRUE);
}

static HtmxResponse *
handle_disks(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	return attach_start(user_data, request, STEP_DISKS, TRUE);
}

static HtmxResponse *
handle_mounts(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	return attach_start(user_data, request, STEP_MOUNTS, TRUE);
}

static HtmxResponse *
handle_create(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	return attach_start(user_data, request, STEP_CREATE, TRUE);
}

/* ── Step 5: waiting for the worker ─────────────────────────────── */

typedef struct
{
	gchar             *name;
	AxisAttachEncoder  encoder;
	GArray            *volumes;	/* gint64: the disks ticked */
	gboolean           fragment;	/* only the part that changes */

	GPtrArray         *disks;
	AxisWorkerInfo    *worker;	/* NULL: there is none of that name */
	GError            *error;
} WaitWork;

static void
wait_work_free(gpointer data)
{
	WaitWork *work = data;

	g_free(work->name);
	g_array_unref(work->volumes);
	g_clear_pointer(&work->disks, g_ptr_array_unref);
	g_clear_pointer(&work->worker, axis_worker_info_free);
	g_clear_error(&work->error);
	g_free(work);
}

/* "3.5.12": the disks' numbers, at most a plan's, none twice. */
static GArray *
volumes_parse(const gchar *text)
{
	GArray *volumes = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_auto(GStrv) parts = NULL;
	guint i;
	guint j;

	if (text == NULL || text[0] == '\0' || strlen(text) > 20 * AXIS_ATTACH_MAX_DISKS)
		return volumes;
	parts = g_strsplit(text, ".", AXIS_ATTACH_MAX_DISKS + 1);
	for (i = 0; parts[i] != NULL; i++) {
		g_autofree gchar *key = g_strdup_printf("disk-%s", parts[i]);
		gint64 number = 0;
		gboolean seen = FALSE;

		if (i >= AXIS_ATTACH_MAX_DISKS || !disk_field_number(key, &number)) {
			g_array_set_size(volumes, 0);
			return volumes;
		}
		for (j = 0; j < volumes->len; j++)
			seen = seen || g_array_index(volumes, gint64, j) == number;
		if (!seen)
			g_array_append_val(volumes, number);
	}
	return volumes;
}

static void
wait_run(
	AxisWebWorkers *self,
	gpointer        data
){
	WaitWork *work = data;
	g_autoptr(GPtrArray) workers = NULL;
	guint i;

	work->disks = disks_load(self->pool, &work->error);
	if (work->disks == NULL || !axis_worker_store_list(self->pool, &workers, &work->error))
		return;
	for (i = 0; i < workers->len; i++) {
		AxisWorkerInfo *worker = g_ptr_array_index(workers, i);

		if (strcmp(worker->name, work->name) == 0) {
			work->worker = g_ptr_array_steal_index(workers, i);
			break;
		}
	}
}

static AxisEncoderFamily
encoder_family(AxisAttachEncoder encoder)
{
	switch (encoder) {
	case AXIS_ATTACH_ENCODER_NVENC:
		return AXIS_ENCODER_NVENC;
	case AXIS_ATTACH_ENCODER_VAAPI:
		return AXIS_ENCODER_VAAPI;
	default:
		return AXIS_ENCODER_SOFTWARE;
	}
}

/* The encoders of the family asked for that the worker measured as usable,
 * comma-separated; NULL when there is none. */
static gchar *
usable_encoders(
	const AxisEncoderCapabilities *caps,
	AxisAttachEncoder              encoder
){
	const AxisConvertEncoder *table = axis_convert_encoders();
	AxisEncoderFamily family = encoder_family(encoder);
	GString *names = g_string_new(NULL);
	guint i;

	for (i = 0; caps != NULL && i < AXIS_CONVERT_N_ENCODERS; i++) {
		if (table[i].family == family && caps->encoders[i].measured && caps->encoders[i].usable)
			g_string_append_printf(names, "%s%s", names->len > 0 ? ", " : "", table[i].name);
	}
	if (names->len == 0) {
		g_string_free(names, TRUE);
		return NULL;
	}
	return g_string_free(names, FALSE);
}

static const AxisWorkerVolumeRow *
proved_row(
	const AxisWorkerInfo *worker,
	const Disk           *disk
){
	guint i;

	for (i = 0; disk != NULL && disk->uuid[0] != '\0' && i < worker->volumes->len; i++) {
		const AxisWorkerVolumeRow *row = g_ptr_array_index(worker->volumes, i);

		if (g_strcmp0(row->uuid, disk->uuid) == 0)
			return row;
	}
	return NULL;
}

static HtmxNode *
cell(const gchar *text)
{
	return HTMX_NODE(htmx_td_new_with_text(text != NULL ? text : ""));
}

static HtmxNode *
cell_of(HtmxNode *child)
{
	HtmxTd *td = htmx_td_new();

	ADD(td, child);
	return HTMX_NODE(td);
}

/* The disks ticked, each proved or not; *@all is whether all are. */
static HtmxNode *
disks_table(
	WaitWork *work,
	gboolean *all
){
	static const AxisWebColumn columns[] = {
		{ "Disk", AXIS_WEB_COL_NAME },
		{ "Proved", AXIS_WEB_COL_STATE },
		{ "Where the worker sees it", AXIS_WEB_COL_NAME },
		{ "What to do", AXIS_WEB_COL_WIDE },
		{ NULL, 0 }
	};
	HtmxNode *table = axis_web_table_new("attach-disks", columns);
	HtmxTbody *body = htmx_tbody_new();
	guint i;

	*all = work->volumes->len > 0;
	for (i = 0; i < work->volumes->len; i++) {
		gint64 volume = g_array_index(work->volumes, gint64, i);
		const Disk *disk = disk_find(work->disks, volume);
		const AxisWorkerVolumeRow *row = proved_row(work->worker, disk);
		g_autofree gchar *inside = axis_attach_container_path(volume);
		HtmxTr *tr = htmx_tr_new();

		ADD(tr, cell(disk != NULL ? disk->name : "A disk Axis does not know any more"));
		if (row != NULL) {
			ADD(tr, cell_of(axis_web_browser_badge("Proved", "badge-succeeded")));
			ADD(tr, cell_of(HTMX_NODE(htmx_code_new_with_text(row->path != NULL ? row->path : ""))));
			ADD(tr, cell(row->keeps_identity ? "Nothing." : "Nothing. The worker checks a file here by its size, its "
				"time to the second and samples of its content, as told for a mount that keeps no file "
				"identities."));
		} else {
			g_autofree gchar *todo = g_strdup_printf("The worker did not find this disk's marker file, .axis-volume, "
				"at %s in its container. On its machine, list the directory you mounted the disk at with \"ls -a\": "
				"it must show .axis-volume. If it does not, the mount is missing, empty or of another disk. If it "
				"does, check that the text you ran names that directory for %s, then restart the worker. A mount "
				"that did not answer when the worker started is proved by itself once it answers.", inside, inside);

			*all = FALSE;
			ADD(tr, cell_of(axis_web_browser_badge("Not proved", "badge-failed")));
			ADD(tr, cell(""));
			ADD(tr, cell(disk != NULL ? todo : "Remove it from the worker's volumes."));
		}
		ADD(body, tr);
	}
	ADD(table, body);
	return table;
}

static const gchar *
encoder_missing(AxisAttachEncoder encoder)
{
	switch (encoder) {
	case AXIS_ATTACH_ENCODER_NVENC:
		return "No NVENC encoder passed its test encode on the worker, so it is given no conversions. On its "
			"machine, check that the NVIDIA driver works (nvidia-smi) and that the NVIDIA Container Toolkit is "
			"installed; with Podman, that the GPU's CDI description exists (\"nvidia-ctk cdi list\" names "
			"nvidia.com/gpu=all; make it with \"sudo nvidia-ctk cdi generate --output=/etc/cdi/nvidia.yaml\"). Then "
			"restart the worker: it measures again when it starts. The table below says why each encoder failed.";
	case AXIS_ATTACH_ENCODER_VAAPI:
		return "No VA-API encoder passed its test encode on the worker, so it is given no conversions. On its "
			"machine, check that /dev/dri/renderD128 is the GPU's render node (\"ls -l /dev/dri\"), that your user is "
			"in the group that owns it (usually render or video), and that the GPU's VA-API driver is installed. "
			"Then restart the worker: it measures again when it starts. The table below says why each encoder "
			"failed.";
	default:
		return "No software encoder passed its test encode on the worker, so it is given no conversions. The "
			"image's ffmpeg has them: look at the worker's log, and at the table below for why each failed.";
	}
}

/* The part of the last page that changes: asked for again when a worker
 * registers and, until everything is good, every few seconds. */
static HtmxNode *
status_panel(
	AxisWebWorkers *self,
	WaitWork       *work,
	const gchar    *query
){
	const AxisWorkerInfo *worker = work->worker;
	HtmxDiv *panel = htmx_div_new();
	g_autofree gchar *source = g_strdup_printf("/workers/attach/status?%s", query);
	g_autofree gchar *encoders = NULL;
	gboolean disks_good = FALSE;
	gboolean good = FALSE;
	gboolean connected;

	htmx_element_set_id(HTMX_ELEMENT(panel), "attach-status");
	htmx_element_set_attribute(HTMX_ELEMENT(panel), "hx-get", source);
	htmx_element_set_attribute(HTMX_ELEMENT(panel), "hx-swap", "outerHTML");

	if (worker->revoked_us > 0) {
		ADD(panel, axis_web_storage_text("notice notice-warning", "This worker was revoked: its token is refused. "
			"Attach another."));
		/* Nothing more will change. */
		good = TRUE;
	} else if (worker->registered_us == 0) {
		gboolean patient = g_get_real_time() - worker->created_us < PATIENCE_US;

		ADD(panel, axis_web_browser_badge("Waiting for the worker", "badge-note"));
		if (worker->refused_protocol >= 0) {
			g_autofree gchar *text = g_strdup_printf("A worker with this token was refused: its axis-worker speaks "
				"worker protocol %d and this server %d. Run the image of this server's version (" AXIS_ATTACH_IMAGE
				":" AXIS_VERSION ").", worker->refused_protocol, AXIS_WORKER_PROTOCOL);

			ADD(panel, axis_web_storage_text("notice notice-warning", text));
		} else if (patient) {
			ADD(panel, axis_web_storage_text(NULL, "Nothing has been heard from it yet. After it starts, a worker "
				"measures its encoders (a short test encode each) and proves its disks, then registers: that takes "
				"from a few seconds to a minute or two. This page updates by itself."));
		} else {
			HtmxUl *list = htmx_ul_new();

			ADD(panel, axis_web_storage_text("notice notice-warning", "The worker has not registered. Look at its "
				"log on its machine, and check:"));
			ADD(list, htmx_li_new_with_text("The address: the worker must reach Axis at the address you gave, from "
				"its machine and from inside its container. Its log says when it cannot connect, and it refuses "
				"plain http to another machine unless it was told to allow it."));
			ADD(list, htmx_li_new_with_text("The token: the file must hold exactly the token step 4 showed, and the "
				"worker's user must be able to read it. Its log says \"the daemon refused this worker's token\" for a "
				"wrong one, and that the token is not in the form the daemon gives for a file that holds something "
				"else."));
			ADD(list, htmx_li_new_with_text("A reverse proxy in front of Axis must pass /api/v1/worker/ on without a "
				"login page and keep the Authorization header."));
			ADD(list, htmx_li_new_with_text("If the token is lost, revoke this worker on the Conversion workers "
				"page and attach another."));
			ADD(panel, list);
		}
	} else {
		HtmxNode *facts = axis_web_browser_facts_new();
		g_autofree gchar *registered = axis_web_storage_format_time(worker->registered_us, "never");

		connected = worker->seen_ago_ms >= 0 && worker->seen_ago_ms <= (gint64)axis_web_workers_connected_ms(self);
		encoders = usable_encoders(worker->caps, work->encoder);
		axis_web_browser_fact_node(facts, "State", worker->paused_us > 0 ?
			axis_web_browser_badge("Paused", "badge-failed") : connected ?
			axis_web_browser_badge("Connected", "badge-succeeded") :
			axis_web_browser_badge("Not heard from", "badge-warn"));
		axis_web_browser_fact(facts, "Registered", registered);
		axis_web_browser_fact(facts, "axis-worker version", worker->worker_version != NULL ? worker->worker_version : "");
		axis_web_browser_fact(facts, "ffmpeg version", worker->ffmpeg_version != NULL ? worker->ffmpeg_version : "");
		axis_web_browser_fact(facts, "Encoders it measured as usable", encoders != NULL ? encoders : "none of the "
			"family you chose");
		ADD(panel, facts);
		if (encoders == NULL)
			ADD(panel, axis_web_storage_text("notice notice-warning", encoder_missing(work->encoder)));
		if (!connected)
			ADD(panel, axis_web_storage_text("notice notice-warning", "It registered, but has not been heard from "
				"recently. Check that its container still runs."));
		ADD(panel, heading(3, "The disks you ticked"));
		if (work->volumes->len > 0)
			ADD(panel, disks_table(work, &disks_good));
		else
			ADD(panel, axis_web_storage_text("help", "This page was opened without the list of disks; the Conversion "
				"workers page shows the disks this worker proved."));
		if (encoders == NULL && worker->caps != NULL) {
			ADD(panel, heading(3, "Encoders this worker measured"));
			ADD(panel, axis_web_workers_encoders_table(worker->caps));
		}
		good = encoders != NULL && disks_good && connected && worker->paused_us == 0;
		if (good) {
			HtmxUl *list = htmx_ul_new();
			HtmxLi *sample = htmx_li_new();
			HtmxLi *run = htmx_li_new();
			HtmxLi *all = htmx_li_new();

			ADD(panel, axis_web_auth_notice(FALSE, "The worker is attached: it is connected, it can encode, and it "
				"proved every disk you ticked."));
			ADD(sample, axis_web_browser_link("/workers#worker-sample", "Measure samples on a worker"));
			ADD(run, axis_web_browser_link("/conversions/runs/new", "Plan a conversion run"));
			ADD(all, axis_web_browser_link("/workers", "Conversion workers"));
			ADD(list, sample);
			ADD(list, run);
			ADD(list, all);
			ADD(panel, list);
		}
	}
	/* Until everything is good the page also asks by itself: an event may
	 * be missed, and "not yet" has to turn into "check this" with time. */
	htmx_element_set_attribute(HTMX_ELEMENT(panel), "hx-trigger", good ?
		"sse:" AXIS_WORKERS_EVENT_CHANGED ", htmx:sseOpen from:body" :
		"sse:" AXIS_WORKERS_EVENT_CHANGED ", htmx:sseOpen from:body, " POLL_TRIGGER);
	return HTMX_NODE(panel);
}

static HtmxResponse *
wait_done(
	AxisWebWorkers *self,
	HtmxRequest    *request,
	gpointer        data
){
	WaitWork *work = data;
	g_autofree gchar *title = g_strdup_printf("Worker %s", work->name);
	GString *query;
	HtmxNode *panel;
	HtmxNode *content;
	HtmxResponse *response;
	guint i;

	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	if (work->worker == NULL)
		return axis_web_error_respond(request, 404, "Not found", "There is no such worker.");

	/* The address of the part that changes, from what was checked. */
	query = g_string_new(NULL);
	g_string_append_printf(query, "name=%s&encoder=%s&disks=", work->name, axis_attach_encoder_word(work->encoder));
	for (i = 0; i < work->volumes->len; i++)
		g_string_append_printf(query, "%s%" G_GINT64_FORMAT, i > 0 ? "." : "", g_array_index(work->volumes, gint64, i));
	panel = status_panel(self, work, query->str);
	g_string_free(query, TRUE);

	if (work->fragment) {
		g_autofree gchar *markup = NULL;

		axis_web_table_apply_roles(panel);
		markup = htmx_element_render(HTMX_ELEMENT(panel));
		g_object_unref(panel);
		response = htmx_response_new();
		htmx_response_set_content(response, markup);
		htmx_response_set_content_type(response, "text/html; charset=utf-8");
		return response;
	}
	axis_web_page_add_topic(request, AXIS_WORKERS_TOPIC);
	content = page_top("Step 5 of 5: wait for the worker. This page shows what the worker reports, and updates by "
		"itself.", NULL);
	{
		HtmxNode *card = axis_web_storage_card(title);

		ADD(card, panel);
		ADD(content, card);
	}
	ADD(content, axis_web_browser_link("/workers", "Conversion workers"));
	response = axis_web_page_respond(request, title, content);
	g_object_unref(content);
	return response;
}

static HtmxResponse *
wait_start(
	AxisWebWorkers *self,
	HtmxRequest    *request,
	gboolean        fragment
){
	const gchar *name = htmx_request_get_query_param(request, "name");
	const gchar *encoder = htmx_request_get_query_param(request, "encoder");
	WaitWork *work;
	AxisAttachEncoder chosen = AXIS_ATTACH_ENCODER_CPU;

	if (!axis_worker_name_is_valid(name))
		return axis_web_error_respond(request, 404, "Not found", "There is no such worker.");
	if (!axis_attach_encoder_from_word(encoder, &chosen))
		return axis_web_error_respond(request, 400, "Bad request", "Open this page from the guide's step 4.");
	work = g_new0(WaitWork, 1);
	work->name = g_strdup(name);
	work->encoder = chosen;
	work->volumes = volumes_parse(htmx_request_get_query_param(request, "disks"));
	work->fragment = fragment;
	return axis_web_workers_dispatch(self, request, wait_run, wait_done, work, wait_work_free);
}

static HtmxResponse *
handle_waiting(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	return wait_start(user_data, request, FALSE);
}

static HtmxResponse *
handle_status(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	const gchar *query = htmx_request_get_query(request);

	/* Nothing to refresh without htmx: send the browser to the page. */
	if (!htmx_request_is_htmx(request)) {
		const gchar *name = htmx_request_get_query_param(request, "name");
		const gchar *encoder = htmx_request_get_query_param(request, "encoder");
		AxisAttachEncoder chosen = AXIS_ATTACH_ENCODER_CPU;
		g_autofree gchar *page = NULL;

		if (!axis_worker_name_is_valid(name) || !axis_attach_encoder_from_word(encoder, &chosen) || query == NULL)
			return axis_web_auth_redirect(request, "/workers");
		/* Only what was checked goes into the address: the disks are left
		 * to the page to ask for again. */
		page = g_strdup_printf("/workers/attach/waiting?name=%s&encoder=%s", name, axis_attach_encoder_word(chosen));
		return axis_web_auth_redirect(request, page);
	}
	return wait_start(user_data, request, TRUE);
}

void
axis_web_workers_attach_register(
	AxisWeb        *web,
	AxisWebWorkers *self
){
	axis_web_route_full(web, HTMX_METHOD_GET, "/workers/attach", AXIS_WEB_ROUTE_ADMIN, handle_first, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/workers/attach", AXIS_WEB_ROUTE_ADMIN, handle_machine, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/workers/attach/disks", AXIS_WEB_ROUTE_ADMIN, handle_disks, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/workers/attach/mounts", AXIS_WEB_ROUTE_ADMIN, handle_mounts, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/workers/attach/create", AXIS_WEB_ROUTE_ADMIN, handle_create, self);
	axis_web_route_full(web, HTMX_METHOD_GET, "/workers/attach/waiting", AXIS_WEB_ROUTE_ADMIN, handle_waiting, self);
	axis_web_route_full(web, HTMX_METHOD_GET, "/workers/attach/status", AXIS_WEB_ROUTE_ADMIN, handle_status, self);
}
