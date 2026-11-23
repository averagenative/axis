/*
 * axis-probe-describe.c - the probe's two ffprobe calls on an open file
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "probe/axis-probe-describe.h"

#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "library/axis-library-name.h"
#include "probe/axis-probe-model.h"
#include "probe/axis-probe-run.h"

static gboolean
run_call(
	const gchar *const *prefix,
	gboolean            frames,
	gint                stream_index,
	gint                fd,
	guint               timeout_ms,
	GCancellable       *cancellable,
	AxisProbeRunResult *result
){
	g_auto(GStrv) argv = axis_probe_tool_argv(prefix, frames, stream_index);
	AxisProbeRunSpec spec;

	if (lseek(fd, 0, SEEK_SET) != 0) {
		memset(result, 0, sizeof(*result));
		result->status = AXIS_PROBE_RUN_SPAWN_FAILED;
		result->output = g_byte_array_new();
		result->errors = g_byte_array_new();
		return FALSE;
	}
	memset(&spec, 0, sizeof(spec));
	spec.argv = (const gchar *const *)argv;
	spec.fd = fd;
	spec.timeout_ms = timeout_ms;
	spec.output_cap = AXIS_PROBE_OUTPUT_CAP;
	spec.error_cap = AXIS_PROBE_STDERR_CAP;
	spec.cancellable = cancellable;
	spec.memory_limit = AXIS_PROBE_MEMORY_LIMIT;
	axis_probe_run(&spec, result);
	return result->status == AXIS_PROBE_RUN_OK;
}

gboolean
axis_probe_describe_fd(
	const gchar *const  *prefix,
	gint                 fd,
	guint                timeout_ms,
	GCancellable        *cancellable,
	AxisMediaInfo      **info_out,
	gchar              **error
){
	g_autoptr(AxisMediaInfo) info = NULL;
	g_autofree gchar *map_error = NULL;
	AxisProbeRunResult result;
	struct stat st;
	gint index = 0;

	*info_out = NULL;
	if (error != NULL)
		*error = NULL;
	if (fstat(fd, &st) != 0) {
		if (error != NULL)
			*error = g_strdup("the file could not be looked at");
		return FALSE;
	}

	if (!run_call(prefix, FALSE, 0, fd, timeout_ms, cancellable, &result)) {
		if (error != NULL) {
			g_autofree gchar *raw = result.errors->len > 0 ?
				g_strndup((const gchar *)result.errors->data, result.errors->len) : NULL;
			g_autofree gchar *said = raw != NULL ? axis_library_text_clean(raw, 200) : NULL;

			*error = g_strdup_printf("ffprobe %s%s%s", axis_probe_run_status_describe(result.status),
				said != NULL ? ": " : "", said != NULL ? said : "");
		}
		axis_probe_run_result_clear(&result);
		return FALSE;
	}
	if (!axis_probe_map_streams((const gchar *)result.output->data, result.output->len, &info, &map_error)) {
		if (error != NULL)
			*error = g_steal_pointer(&map_error);
		axis_probe_run_result_clear(&result);
		return FALSE;
	}
	axis_probe_run_result_clear(&result);

	if (axis_media_info_wants_frames(info, &index)) {
		if (run_call(prefix, TRUE, index, fd, timeout_ms, cancellable, &result))
			axis_probe_map_frames(info, (const gchar *)result.output->data, result.output->len);
		else
			axis_probe_map_frames(info, NULL, 0);
		axis_probe_run_result_clear(&result);
	}
	axis_media_info_finish(info, (gint64)st.st_size);
	info->status = g_strdup("ok");
	*info_out = g_steal_pointer(&info);
	return TRUE;
}
