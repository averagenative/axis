/*
 * axis-probe.c - the probe object: settings, the tool and its arguments
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "probe/axis-probe-private.h"

#include <string.h>

#include "library/axis-library-name.h"

G_DEFINE_QUARK(axis-probe-error-quark, axis_probe_error)

/* ── Settings ───────────────────────────────────────────────────── */

static const AxisSettingInfo setting_table[] = {
	{
		AXIS_PROBE_SETTING_PATH, AXIS_SETTING_STRING,
		"ffprobe for reading files",
		"The ffprobe program that looks inside media files: a name found on Axis's PATH, "
		"or an absolute path. Version 6 or newer.",
		0, "ffprobe",
		1, 255, 0, 0, NULL
	},
	{
		AXIS_PROBE_SETTING_TIMEOUT, AXIS_SETTING_DURATION,
		"Time limit for reading one file",
		"How long ffprobe may take on one file before it is stopped and the file is recorded as "
		"timed out. Slow or failing disks need more.",
		0, "60s",
		5, 3600, 0, 0, NULL
	},
	{ NULL, 0, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL }
};

const AxisSettingInfo *
axis_probe_get_setting_table(void)
{
	return setting_table;
}

/* ── Lifetime ───────────────────────────────────────────────────── */

AxisProbe *
axis_probe_new(
	AxisDbPool   *pool,
	AxisStorage  *storage,
	AxisSettings *settings
){
	AxisProbe *self;

	g_return_val_if_fail(pool != NULL, NULL);
	g_return_val_if_fail(storage != NULL, NULL);

	self = g_new0(AxisProbe, 1);
	self->ref_count = 1;
	self->pool = pool;
	self->storage = axis_storage_ref(storage);
	self->settings = settings != NULL ? axis_settings_ref(settings) : NULL;
	g_mutex_init(&self->lock);

	return self;
}

AxisProbe *
axis_probe_ref(AxisProbe *self)
{
	g_return_val_if_fail(self != NULL, NULL);

	g_atomic_int_inc(&self->ref_count);
	return self;
}

void
axis_probe_unref(AxisProbe *self)
{
	if (self == NULL || !g_atomic_int_dec_and_test(&self->ref_count))
		return;

	g_strfreev(self->tool_command);
	g_free(self->version_program);
	g_free(self->version_text);
	axis_storage_unref(self->storage);
	if (self->settings != NULL)
		axis_settings_unref(self->settings);
	g_mutex_clear(&self->lock);
	g_free(self);
}

void
axis_probe_set_tuning(
	AxisProbe             *self,
	const AxisProbeTuning *tuning
){
	g_return_if_fail(self != NULL);

	g_mutex_lock(&self->lock);
	g_strfreev(self->tool_command);
	self->tool_command = NULL;
	if (tuning != NULL) {
		self->tuning = *tuning;
		if (tuning->tool_command != NULL) {
			self->tool_command = g_strdupv((gchar **)tuning->tool_command);
			self->tuning.tool_command = (const gchar *const *)self->tool_command;
		}
	} else {
		memset(&self->tuning, 0, sizeof(self->tuning));
	}
	g_mutex_unlock(&self->lock);
}

guint64
axis_probe_get_runs(AxisProbe *self)
{
	g_return_val_if_fail(self != NULL, 0);

	return (guint64)(guint)g_atomic_int_get(&self->runs);
}

gint
axis_probe_current_version(AxisProbe *self)
{
	gint version;

	g_mutex_lock(&self->lock);
	version = self->tuning.version > 0 ? self->tuning.version : AXIS_PROBE_VERSION;
	g_mutex_unlock(&self->lock);
	return version;
}

guint
axis_probe_get_timeout_ms(AxisProbe *self)
{
	guint ms = AXIS_PROBE_DEFAULT_TIMEOUT_SECONDS * 1000;

	g_mutex_lock(&self->lock);
	if (self->tuning.timeout_ms > 0)
		ms = self->tuning.timeout_ms;
	else if (self->settings != NULL &&
	         axis_settings_lookup(self->settings, AXIS_PROBE_SETTING_TIMEOUT, NULL) != NULL)
		ms = (guint)CLAMP(axis_settings_get_duration(self->settings, AXIS_PROBE_SETTING_TIMEOUT), 5,
			3600) * 1000;
	g_mutex_unlock(&self->lock);
	return ms;
}

/* ── The tool ───────────────────────────────────────────────────── */

gchar *
axis_probe_resolve_tool(AxisProbe *self)
{
	g_autofree gchar *name = NULL;
	gchar *program = NULL;

	g_mutex_lock(&self->lock);
	if (self->tool_command != NULL && self->tool_command[0] != NULL)
		program = g_strdup(self->tool_command[0]);
	g_mutex_unlock(&self->lock);
	if (program != NULL)
		return program;

	if (self->settings != NULL &&
	    axis_settings_lookup(self->settings, AXIS_PROBE_SETTING_PATH, NULL) != NULL)
		name = axis_settings_get_string(self->settings, AXIS_PROBE_SETTING_PATH);
	if (name == NULL || name[0] == '\0') {
		g_free(name);
		name = g_strdup("ffprobe");
	}

	if (strchr(name, '/') != NULL) {
		if (g_path_is_absolute(name) && g_file_test(name, G_FILE_TEST_IS_REGULAR | G_FILE_TEST_IS_EXECUTABLE))
			return g_steal_pointer(&name);
		return NULL;
	}
	return g_find_program_in_path(name);
}

gchar *
axis_probe_tool_version(
	AxisProbe   *self,
	const gchar *program
){
	AxisProbeRunSpec spec;
	AxisProbeRunResult result;
	const gchar *argv[3];
	g_autofree gchar *text = NULL;
	gchar *version = NULL;
	gboolean test_tool;

	g_mutex_lock(&self->lock);
	test_tool = self->tool_command != NULL;
	if (!test_tool && g_strcmp0(self->version_program, program) == 0 && self->version_text != NULL)
		version = g_strdup(self->version_text);
	g_mutex_unlock(&self->lock);
	if (test_tool || version != NULL)
		return version;

	argv[0] = program;
	argv[1] = "-version";
	argv[2] = NULL;
	memset(&spec, 0, sizeof(spec));
	spec.argv = (const gchar *const *)argv;
	spec.fd = -1;
	spec.timeout_ms = 10000;
	spec.output_cap = 64 * 1024;
	spec.error_cap = 4096;
	spec.memory_limit = AXIS_PROBE_MEMORY_LIMIT;
	axis_probe_run(&spec, &result);

	if (result.status == AXIS_PROBE_RUN_OK && result.output->len > 0) {
		/* "ffprobe version 8.1.3 Copyright ...": the word after "version". */
		g_autofree gchar *head = g_strndup((const gchar *)result.output->data, MIN(result.output->len, 200u));
		gchar *at = strstr(head, " version ");

		if (at != NULL) {
			gchar *end;

			at += strlen(" version ");
			end = at;
			while (*end != '\0' && !g_ascii_isspace(*end))
				end++;
			*end = '\0';
			text = axis_library_text_clean(at, 64);
		}
	}
	axis_probe_run_result_clear(&result);

	if (text != NULL && text[0] != '\0') {
		g_mutex_lock(&self->lock);
		g_free(self->version_program);
		g_free(self->version_text);
		self->version_program = g_strdup(program);
		self->version_text = g_strdup(text);
		g_mutex_unlock(&self->lock);
		return g_steal_pointer(&text);
	}
	return NULL;
}

gchar **
axis_probe_build_argv(
	AxisProbe   *self,
	const gchar *program,
	gboolean     frames,
	gint         stream_index
){
	g_auto(GStrv) prefix = NULL;
	const gchar *own[2];

	g_mutex_lock(&self->lock);
	prefix = self->tool_command != NULL ? g_strdupv(self->tool_command) : NULL;
	g_mutex_unlock(&self->lock);

	own[0] = program;
	own[1] = NULL;
	return axis_probe_tool_argv(prefix != NULL ? (const gchar *const *)prefix : own, frames, stream_index);
}

void
axis_probe_run_tool(
	AxisProbe          *self,
	gchar             **argv,
	gint                fd,
	GCancellable       *cancellable,
	AxisProbeRunResult *result
){
	AxisProbeRunSpec spec;
	guint timeout;
	gsize cap;
	guint64 memory;

	g_mutex_lock(&self->lock);
	cap = self->tuning.output_cap > 0 ? self->tuning.output_cap : AXIS_PROBE_OUTPUT_CAP;
	memory = self->tuning.memory_limit > 0 ? self->tuning.memory_limit : AXIS_PROBE_MEMORY_LIMIT;
	g_mutex_unlock(&self->lock);
	timeout = axis_probe_get_timeout_ms(self);

	memset(&spec, 0, sizeof(spec));
	spec.argv = (const gchar *const *)argv;
	spec.fd = fd;
	spec.timeout_ms = timeout;
	spec.output_cap = cap;
	spec.error_cap = AXIS_PROBE_STDERR_CAP;
	spec.cancellable = cancellable;
	spec.memory_limit = memory;

	g_atomic_int_inc(&self->runs);
	axis_probe_run(&spec, result);
}

/* ── The target ─────────────────────────────────────────────────── */

void
axis_probe_target_clear(AxisProbeTarget *target)
{
	g_free(target->rel_path);
	g_free(target->root_rel_path);
	g_free(target->kind);
	g_free(target->state);
	memset(target, 0, sizeof(*target));
}

gboolean
axis_probe_target_pending(
	const AxisProbeTarget *target,
	gint                   current_version
){
	if (g_strcmp0(target->kind, "video") != 0 && g_strcmp0(target->kind, "audio") != 0)
		return FALSE;
	if (g_strcmp0(target->state, "present") != 0)
		return FALSE;
	if (target->changed && target->changed_generation <= target->complete_generation)
		return TRUE;
	return target->has_info && target->info_version < current_version;
}
