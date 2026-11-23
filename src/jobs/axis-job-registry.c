/*
 * axis-job-registry.c - job kinds, their handlers and default schedules
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "jobs/axis-job-registry.h"
#include "jobs/axis-job-error.h"
#include "jobs/axis-job-lifecycle.h"
#include "jobs/axis-job-private.h"

#include <string.h>

/* A copied AxisJobScheduleInfo, owning its strings. */
typedef struct
{
	AxisJobScheduleInfo info;
	gchar              *name;
	gchar              *kind;
	gchar              *payload;
} Schedule;

struct _AxisJobRegistry
{
	gint        ref_count;	/* atomic */
	GMutex      lock;
	GHashTable *kinds;	/* (lock) kind -> AxisJobKind */
	GPtrArray  *schedules;	/* (lock) of Schedule, in the order added */
	gboolean    frozen;	/* (lock) */
};

/* ── Holds ──────────────────────────────────────────────────────── */

struct _AxisJobHold
{
	gint ref_count;	/* atomic */
	gint held;	/* atomic */
};

AxisJobHold *
axis_job_hold_new(void)
{
	AxisJobHold *self = g_new0(AxisJobHold, 1);

	self->ref_count = 1;
	return self;
}

AxisJobHold *
axis_job_hold_ref(AxisJobHold *self)
{
	g_return_val_if_fail(self != NULL, NULL);

	g_atomic_int_inc(&self->ref_count);
	return self;
}

void
axis_job_hold_unref(AxisJobHold *self)
{
	if (self == NULL || !g_atomic_int_dec_and_test(&self->ref_count))
		return;
	g_free(self);
}

void
axis_job_hold_set(
	AxisJobHold *self,
	gboolean     held
){
	g_return_if_fail(self != NULL);

	g_atomic_int_set(&self->held, held ? 1 : 0);
}

gboolean
axis_job_hold_is_held(AxisJobHold *self)
{
	return self != NULL && g_atomic_int_get(&self->held) != 0;
}

/* ── Kinds ──────────────────────────────────────────────────────── */

static void
kind_free(gpointer data)
{
	AxisJobKind *kind = data;

	g_free(kind->kind);
	g_free(kind->queue);
	g_free(kind->label);
	g_free(kind->retry_elsewhere);
	axis_module_switch_unref(kind->module_switch);
	axis_job_hold_unref(kind->hold);
	g_free(kind);
}

static void
schedule_free(gpointer data)
{
	Schedule *schedule = data;

	g_free(schedule->name);
	g_free(schedule->kind);
	g_free(schedule->payload);
	g_free(schedule);
}

AxisJobRegistry *
axis_job_registry_new(void)
{
	AxisJobRegistry *self = g_new0(AxisJobRegistry, 1);

	self->ref_count = 1;
	g_mutex_init(&self->lock);
	self->kinds = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, kind_free);
	self->schedules = g_ptr_array_new_with_free_func(schedule_free);

	return self;
}

AxisJobRegistry *
axis_job_registry_ref(AxisJobRegistry *self)
{
	g_return_val_if_fail(self != NULL, NULL);

	g_atomic_int_inc(&self->ref_count);
	return self;
}

void
axis_job_registry_unref(AxisJobRegistry *self)
{
	if (self == NULL)
		return;
	if (!g_atomic_int_dec_and_test(&self->ref_count))
		return;

	g_hash_table_unref(self->kinds);
	g_ptr_array_unref(self->schedules);
	g_mutex_clear(&self->lock);
	g_free(self);
}

static gboolean
check_name(
	const gchar  *what,
	const gchar  *name,
	GError      **error
){
	if (axis_job_name_is_valid(name))
		return TRUE;

	g_set_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_INVALID,
		"invalid %s name '%s' (use 1 to %d lower-case letters, digits, '.', '_' and '-', "
		"starting with a letter)", what, name != NULL ? name : "(null)", AXIS_JOB_NAME_MAX_LEN);
	return FALSE;
}

/* A label is one line of plain text, or none (only a test's kinds go
 * without; the daemon refuses them, see axis_job_registry_list_undescribed()). */
static gboolean
check_description(
	const gchar   *kind,
	const gchar   *label,
	AxisJobClass   job_class,
	GError       **error
){
	if (job_class != AXIS_JOB_CLASS_UNSET && job_class != AXIS_JOB_CLASS_WORK &&
	    job_class != AXIS_JOB_CLASS_UPKEEP) {
		g_set_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_INVALID,
			"job kind '%s' has an unknown class %d", kind, (gint)job_class);
		return FALSE;
	}
	if (label != NULL && (label[0] == '\0' || strlen(label) > AXIS_JOB_LABEL_MAX_LEN ||
	                      !g_utf8_validate(label, -1, NULL) || strpbrk(label, "\r\n\t") != NULL)) {
		g_set_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_INVALID,
			"job kind '%s' has a label that is not one line of 1 to %d bytes of text", kind,
			AXIS_JOB_LABEL_MAX_LEN);
		return FALSE;
	}
	return TRUE;
}

/* Where a kind is retried instead is a sentence for a page, or none. */
static gboolean
check_retry_elsewhere(
	const gchar  *kind,
	const gchar  *text,
	GError      **error
){
	if (text == NULL || (text[0] != '\0' && strlen(text) <= AXIS_JOB_RETRY_ELSEWHERE_MAX_LEN &&
	                     g_utf8_validate(text, -1, NULL) && strpbrk(text, "\r\n\t") == NULL))
		return TRUE;

	g_set_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_INVALID,
		"job kind '%s' says where it is retried in something that is not one line of 1 to %d bytes of text",
		kind, AXIS_JOB_RETRY_ELSEWHERE_MAX_LEN);
	return FALSE;
}

static gboolean
check_not_frozen_locked(
	AxisJobRegistry  *self,
	GError          **error
){
	if (!self->frozen)
		return TRUE;

	g_set_error_literal(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_STATE,
		"the job registry is in use by a running queue; nothing can be added to it");
	return FALSE;
}

gboolean
axis_job_registry_add(
	AxisJobRegistry           *self,
	const AxisJobHandlerInfo  *info,
	gpointer                   user_data,
	GError                   **error
){
	AxisJobKind *kind;
	guint max_attempts;

	g_return_val_if_fail(self != NULL, FALSE);
	g_return_val_if_fail(info != NULL, FALSE);

	if (!check_name("job kind", info->kind, error) || !check_name("queue", info->queue, error) ||
	    !check_description(info->kind, info->label, info->job_class, error))
		return FALSE;
	if (info->handler == NULL) {
		g_set_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_INVALID,
			"job kind '%s' has no handler", info->kind);
		return FALSE;
	}
	max_attempts = info->max_attempts != 0 ? info->max_attempts : AXIS_JOB_DEFAULT_MAX_ATTEMPTS;
	if (max_attempts > AXIS_JOB_MAX_ATTEMPTS_LIMIT) {
		g_set_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_INVALID,
			"job kind '%s': max_attempts must be from 1 to %d, not %u",
			info->kind, AXIS_JOB_MAX_ATTEMPTS_LIMIT, max_attempts);
		return FALSE;
	}

	g_mutex_lock(&self->lock);
	if (!check_not_frozen_locked(self, error)) {
		g_mutex_unlock(&self->lock);
		return FALSE;
	}
	if (g_hash_table_contains(self->kinds, info->kind)) {
		g_mutex_unlock(&self->lock);
		g_set_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_INVALID,
			"job kind '%s' is already registered", info->kind);
		return FALSE;
	}

	/* A queue is served in-process or by remote workers, never both. */
	{
		GHashTableIter iter;
		gpointer value;

		g_hash_table_iter_init(&iter, self->kinds);
		while (g_hash_table_iter_next(&iter, NULL, &value)) {
			const AxisJobKind *other = value;

			if (other->remote && strcmp(other->queue, info->queue) == 0) {
				g_mutex_unlock(&self->lock);
				g_set_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_INVALID,
					"job kind '%s': queue '%s' is the queue of remote kinds", info->kind, info->queue);
				return FALSE;
			}
		}
	}

	kind = g_new0(AxisJobKind, 1);
	kind->kind = g_strdup(info->kind);
	kind->queue = g_strdup(info->queue);
	kind->max_attempts = max_attempts;
	kind->handler = info->handler;
	kind->user_data = user_data;
	kind->label = g_strdup(info->label);
	kind->job_class = info->job_class;
	g_hash_table_insert(self->kinds, kind->kind, kind);
	g_mutex_unlock(&self->lock);

	return TRUE;
}

gboolean
axis_job_registry_add_remote(
	AxisJobRegistry   *self,
	const gchar       *kind_name,
	const gchar       *queue,
	guint              max_attempts,
	const gchar       *label,
	AxisJobClass       job_class,
	AxisModuleSwitch  *module_switch,
	GError           **error
){
	AxisJobKind *kind;
	guint attempts;

	g_return_val_if_fail(self != NULL, FALSE);

	if (!check_name("job kind", kind_name, error) || !check_name("queue", queue, error) ||
	    !check_description(kind_name, label, job_class, error)) {
		axis_module_switch_unref(module_switch);
		return FALSE;
	}
	attempts = max_attempts != 0 ? max_attempts : AXIS_JOB_DEFAULT_MAX_ATTEMPTS;
	if (attempts > AXIS_JOB_MAX_ATTEMPTS_LIMIT) {
		axis_module_switch_unref(module_switch);
		g_set_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_INVALID,
			"job kind '%s': max_attempts must be from 1 to %d, not %u",
			kind_name, AXIS_JOB_MAX_ATTEMPTS_LIMIT, attempts);
		return FALSE;
	}

	g_mutex_lock(&self->lock);
	if (!check_not_frozen_locked(self, error)) {
		g_mutex_unlock(&self->lock);
		axis_module_switch_unref(module_switch);
		return FALSE;
	}
	if (g_hash_table_contains(self->kinds, kind_name)) {
		g_mutex_unlock(&self->lock);
		axis_module_switch_unref(module_switch);
		g_set_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_INVALID,
			"job kind '%s' is already registered", kind_name);
		return FALSE;
	}
	{
		GHashTableIter iter;
		gpointer value;

		g_hash_table_iter_init(&iter, self->kinds);
		while (g_hash_table_iter_next(&iter, NULL, &value)) {
			const AxisJobKind *other = value;

			if (!other->remote && strcmp(other->queue, queue) == 0) {
				g_mutex_unlock(&self->lock);
				axis_module_switch_unref(module_switch);
				g_set_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_INVALID,
					"remote job kind '%s': queue '%s' is the queue of in-process kinds", kind_name, queue);
				return FALSE;
			}
		}
	}

	kind = g_new0(AxisJobKind, 1);
	kind->kind = g_strdup(kind_name);
	kind->queue = g_strdup(queue);
	kind->max_attempts = attempts;
	kind->remote = TRUE;
	kind->module_switch = module_switch;
	kind->label = g_strdup(label);
	kind->job_class = job_class;
	g_hash_table_insert(self->kinds, kind->kind, kind);
	g_mutex_unlock(&self->lock);

	return TRUE;
}

const gchar *
axis_job_registry_get_label(
	AxisJobRegistry *self,
	const gchar     *kind
){
	const AxisJobKind *entry = axis_job_registry_lookup(self, kind);

	return entry != NULL ? entry->label : NULL;
}

AxisJobClass
axis_job_registry_get_class(
	AxisJobRegistry *self,
	const gchar     *kind
){
	const AxisJobKind *entry = axis_job_registry_lookup(self, kind);

	return entry != NULL ? entry->job_class : AXIS_JOB_CLASS_UNSET;
}

const gchar *
axis_job_registry_get_retry_elsewhere(
	AxisJobRegistry *self,
	const gchar     *kind
){
	const AxisJobKind *entry = axis_job_registry_lookup(self, kind);

	return entry != NULL ? entry->retry_elsewhere : NULL;
}

gboolean
axis_job_registry_set_retry_elsewhere(
	AxisJobRegistry  *self,
	const gchar      *kind,
	const gchar      *text,
	GError          **error
){
	AxisJobKind *entry;

	g_return_val_if_fail(self != NULL, FALSE);

	if (!check_retry_elsewhere(kind, text, error))
		return FALSE;
	g_mutex_lock(&self->lock);
	if (!check_not_frozen_locked(self, error)) {
		g_mutex_unlock(&self->lock);
		return FALSE;
	}
	entry = kind != NULL ? g_hash_table_lookup(self->kinds, kind) : NULL;
	if (entry == NULL) {
		g_mutex_unlock(&self->lock);
		g_set_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_UNKNOWN_KIND, "no job kind '%s' is registered",
			kind != NULL ? kind : "(null)");
		return FALSE;
	}
	g_free(entry->retry_elsewhere);
	entry->retry_elsewhere = g_strdup(text);
	g_mutex_unlock(&self->lock);
	return TRUE;
}

/* The kinds, sorted, of @job_class, or (@undescribed) of no label or no
 * class. */
static GStrv
list_kinds_where(
	AxisJobRegistry *self,
	AxisJobClass     job_class,
	gboolean         undescribed
){
	g_autoptr(GPtrArray) kinds = axis_job_registry_list_kinds(self);
	GPtrArray *names = g_ptr_array_new();
	guint i;

	for (i = 0; i < kinds->len; i++) {
		const AxisJobKind *kind = g_ptr_array_index(kinds, i);
		gboolean wanted = undescribed ? kind->label == NULL || kind->job_class == AXIS_JOB_CLASS_UNSET :
			kind->job_class == job_class;

		if (wanted)
			g_ptr_array_add(names, g_strdup(kind->kind));
	}
	g_ptr_array_add(names, NULL);
	return (GStrv)g_ptr_array_free(names, FALSE);
}

GStrv
axis_job_registry_list_class(
	AxisJobRegistry *self,
	AxisJobClass     job_class
){
	g_return_val_if_fail(self != NULL, NULL);

	return list_kinds_where(self, job_class, FALSE);
}

GStrv
axis_job_registry_list_undescribed(AxisJobRegistry *self)
{
	g_return_val_if_fail(self != NULL, NULL);

	return list_kinds_where(self, AXIS_JOB_CLASS_UNSET, TRUE);
}

gboolean
axis_job_registry_is_remote(
	AxisJobRegistry *self,
	const gchar     *kind
){
	const AxisJobKind *entry = axis_job_registry_lookup(self, kind);

	return entry != NULL && entry->remote;
}

gboolean
axis_job_registry_kind_claimable(
	AxisJobRegistry *self,
	const gchar     *kind
){
	const AxisJobKind *entry = axis_job_registry_lookup(self, kind);

	return entry != NULL && axis_job_registry_kind_enabled(self, entry);
}

static gboolean
payload_is_object(const gchar *payload)
{
	g_autoptr(JsonParser) parser = json_parser_new();

	if (payload == NULL)
		return TRUE;
	if (!json_parser_load_from_data(parser, payload, -1, NULL))
		return FALSE;

	return JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser));
}

gboolean
axis_job_registry_add_schedule(
	AxisJobRegistry            *self,
	const AxisJobScheduleInfo  *info,
	GError                    **error
){
	Schedule *schedule;
	guint i;

	g_return_val_if_fail(self != NULL, FALSE);
	g_return_val_if_fail(info != NULL, FALSE);

	if (!check_name("schedule", info->name, error) || !check_name("job kind", info->kind, error))
		return FALSE;
	if (info->interval_seconds == 0) {
		g_set_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_INVALID,
			"schedule '%s' has an interval of 0 seconds", info->name);
		return FALSE;
	}
	if (!payload_is_object(info->payload)) {
		g_set_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_INVALID,
			"schedule '%s': the payload is not a JSON object", info->name);
		return FALSE;
	}

	g_mutex_lock(&self->lock);
	if (!check_not_frozen_locked(self, error)) {
		g_mutex_unlock(&self->lock);
		return FALSE;
	}
	if (!g_hash_table_contains(self->kinds, info->kind)) {
		g_mutex_unlock(&self->lock);
		g_set_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_INVALID,
			"schedule '%s' is for job kind '%s', which is not registered",
			info->name, info->kind);
		return FALSE;
	}
	for (i = 0; i < self->schedules->len; i++) {
		Schedule *existing = g_ptr_array_index(self->schedules, i);

		if (strcmp(existing->name, info->name) == 0) {
			g_mutex_unlock(&self->lock);
			g_set_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_INVALID,
				"schedule '%s' is already registered", info->name);
			return FALSE;
		}
	}

	schedule = g_new0(Schedule, 1);
	schedule->name = g_strdup(info->name);
	schedule->kind = g_strdup(info->kind);
	schedule->payload = g_strdup(info->payload);
	schedule->info.name = schedule->name;
	schedule->info.kind = schedule->kind;
	schedule->info.payload = schedule->payload;
	schedule->info.interval_seconds = info->interval_seconds;
	g_ptr_array_add(self->schedules, schedule);
	g_mutex_unlock(&self->lock);

	return TRUE;
}

gboolean
axis_job_registry_add_modules(
	AxisJobRegistry     *self,
	AxisModuleRegistry  *modules,
	GError             **error
){
	guint n;
	guint i;

	g_return_val_if_fail(self != NULL, FALSE);
	g_return_val_if_fail(modules != NULL, FALSE);

	n = axis_module_registry_get_count(modules);

	/* Every module's kinds first: a schedule may be for another module's. */
	for (i = 0; i < n; i++) {
		const AxisJobHandlerInfo *handlers = axis_module_registry_get_job_handlers(modules, i);
		gpointer user_data = axis_module_registry_get_user_data(modules, i);
		guint j;

		for (j = 0; handlers != NULL && handlers[j].kind != NULL; j++) {
			g_autoptr(GError) local = NULL;

			if (!axis_job_registry_add(self, &handlers[j], user_data, &local)) {
				g_propagate_prefixed_error(error, g_steal_pointer(&local), "module %s: ",
					axis_module_registry_get_id(modules, i));
				return FALSE;
			}

			/* From now on the kind follows its module's switch. */
			g_mutex_lock(&self->lock);
			{
				AxisJobKind *kind = g_hash_table_lookup(self->kinds, handlers[j].kind);

				kind->module_switch = axis_module_registry_ref_switch(modules,
					axis_module_registry_get_id(modules, i));
			}
			g_mutex_unlock(&self->lock);
		}
	}

	for (i = 0; i < n; i++) {
		const AxisJobScheduleInfo *schedules = axis_module_registry_get_job_schedules(modules, i);
		guint j;

		for (j = 0; schedules != NULL && schedules[j].name != NULL; j++) {
			g_autoptr(GError) local = NULL;

			if (!axis_job_registry_add_schedule(self, &schedules[j], &local)) {
				g_propagate_prefixed_error(error, g_steal_pointer(&local), "module %s: ",
					axis_module_registry_get_id(modules, i));
				return FALSE;
			}
		}
	}

	/* Pairs of a kind and where its work is asked for again. */
	for (i = 0; i < n; i++) {
		const gchar *const *notes = axis_module_registry_get_job_retry_elsewhere(modules, i);
		guint j;

		for (j = 0; notes != NULL && notes[j] != NULL; j += 2) {
			g_autoptr(GError) local = NULL;

			/* A kind with no sentence ends the table early: the next
			 * pair would be read past its end. */
			if (notes[j + 1] == NULL) {
				g_set_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_INVALID,
					"module %s: job kind '%s' is retried elsewhere, but says nowhere where",
					axis_module_registry_get_id(modules, i), notes[j]);
				return FALSE;
			}
			if (!axis_job_registry_set_retry_elsewhere(self, notes[j], notes[j + 1], &local)) {
				g_propagate_prefixed_error(error, g_steal_pointer(&local), "module %s: ",
					axis_module_registry_get_id(modules, i));
				return FALSE;
			}
		}
	}

	return TRUE;
}

gboolean
axis_job_registry_contains(
	AxisJobRegistry *self,
	const gchar     *kind
){
	return axis_job_registry_lookup(self, kind) != NULL;
}

gboolean
axis_job_registry_enqueue(
	AxisJobRegistry              *self,
	AxisDbConn                   *conn,
	const gchar                  *kind,
	JsonObject                   *payload,
	const AxisJobEnqueueOptions  *options,
	gint64                       *id,
	gboolean                     *created,
	GError                      **error
){
	const AxisJobKind *entry;
	AxisJobSpec spec;

	g_return_val_if_fail(self != NULL, FALSE);
	g_return_val_if_fail(conn != NULL, FALSE);

	entry = axis_job_registry_lookup(self, kind);
	if (entry == NULL) {
		g_set_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_UNKNOWN_KIND,
			"cannot enqueue a job of kind '%s': no handler is registered for it",
			kind != NULL ? kind : "(null)");
		return FALSE;
	}

	memset(&spec, 0, sizeof(spec));
	spec.kind = entry->kind;
	spec.queue = entry->queue;
	spec.payload = payload;
	spec.max_attempts = entry->max_attempts;
	if (options != NULL) {
		spec.priority = options->priority;
		spec.delay_ms = options->delay_ms;
		spec.dedupe_key = options->dedupe_key;
		if (options->max_attempts != 0)
			spec.max_attempts = options->max_attempts;
	}

	return axis_job_enqueue(conn, &spec, id, created, error);
}

/*
 * Internals
 */

void
axis_job_registry_freeze(AxisJobRegistry *self)
{
	g_return_if_fail(self != NULL);

	g_mutex_lock(&self->lock);
	self->frozen = TRUE;
	g_mutex_unlock(&self->lock);
}

const AxisJobKind *
axis_job_registry_lookup(
	AxisJobRegistry *self,
	const gchar     *kind
){
	const AxisJobKind *entry;

	g_return_val_if_fail(self != NULL, NULL);

	if (kind == NULL)
		return NULL;

	g_mutex_lock(&self->lock);
	entry = g_hash_table_lookup(self->kinds, kind);
	g_mutex_unlock(&self->lock);

	return entry;
}

gboolean
axis_job_registry_kind_enabled(
	AxisJobRegistry   *self,
	const AxisJobKind *kind
){
	g_return_val_if_fail(self != NULL, FALSE);
	g_return_val_if_fail(kind != NULL, FALSE);

	/* The switch and the hold are set before any queue uses the kind. */
	return (kind->module_switch == NULL || axis_module_switch_is_on(kind->module_switch)) &&
		!axis_job_hold_is_held(kind->hold);
}

gboolean
axis_job_registry_set_hold(
	AxisJobRegistry  *self,
	const gchar      *kind,
	AxisJobHold      *hold,
	GError          **error
){
	AxisJobKind *entry;

	g_return_val_if_fail(self != NULL, FALSE);

	g_mutex_lock(&self->lock);
	if (self->frozen) {
		g_mutex_unlock(&self->lock);
		g_set_error_literal(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_STATE,
			"a hold is set before the queue starts");
		return FALSE;
	}
	entry = kind != NULL ? g_hash_table_lookup(self->kinds, kind) : NULL;
	if (entry == NULL) {
		g_mutex_unlock(&self->lock);
		g_set_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_UNKNOWN_KIND, "no job kind '%s' is registered",
			kind != NULL ? kind : "(null)");
		return FALSE;
	}
	axis_job_hold_unref(entry->hold);
	entry->hold = hold != NULL ? axis_job_hold_ref(hold) : NULL;
	g_mutex_unlock(&self->lock);
	return TRUE;
}

static gint
compare_kinds(
	gconstpointer a,
	gconstpointer b
){
	const AxisJobKind *x = *(const AxisJobKind *const *)a;
	const AxisJobKind *y = *(const AxisJobKind *const *)b;

	return strcmp(x->kind, y->kind);
}

GPtrArray *
axis_job_registry_list_kinds(AxisJobRegistry *self)
{
	GPtrArray *list;
	GHashTableIter iter;
	gpointer value;

	g_return_val_if_fail(self != NULL, NULL);

	list = g_ptr_array_new();
	g_mutex_lock(&self->lock);
	g_hash_table_iter_init(&iter, self->kinds);
	while (g_hash_table_iter_next(&iter, NULL, &value))
		g_ptr_array_add(list, value);
	g_mutex_unlock(&self->lock);

	g_ptr_array_sort(list, compare_kinds);
	return list;
}

GPtrArray *
axis_job_registry_list_schedules(AxisJobRegistry *self)
{
	GPtrArray *list;
	guint i;

	g_return_val_if_fail(self != NULL, NULL);

	list = g_ptr_array_new();
	g_mutex_lock(&self->lock);
	for (i = 0; i < self->schedules->len; i++) {
		Schedule *schedule = g_ptr_array_index(self->schedules, i);

		g_ptr_array_add(list, &schedule->info);
	}
	g_mutex_unlock(&self->lock);

	return list;
}
