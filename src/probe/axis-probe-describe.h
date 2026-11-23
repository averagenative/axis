/*
 * axis-probe-describe.h - what ffprobe says about an open file that is not
 * one of the library's (an encoder's output, a test clip), read with the
 * probe's own two calls and mapper
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef AXIS_PROBE_DESCRIBE_H
#define AXIS_PROBE_DESCRIBE_H

#include <gio/gio.h>
#include <glib.h>

#include "probe/axis-probe-model.h"

G_BEGIN_DECLS

/**
 * axis_probe_describe_fd:
 * @prefix: ffprobe, and any arguments before the probe's own, ended by
 *   %NULL
 * @fd: the file, open for reading (read from its start; the caller keeps
 *   it)
 * @timeout_ms: for each of the two calls
 * @cancellable: (nullable): stops the tool
 * @info: (out) (transfer full): the record, classified and finished
 * @error: (out) (transfer full) (optional): why not, a sentence
 *
 * Runs axis_probe_tool_argv()'s calls under the probe's limits (the file
 * as descriptor 3, never a name), the frame call when the mapper wants it,
 * and maps them as the probe does.  Blocks.
 *
 * Returns: %TRUE with a record
 */
gboolean
axis_probe_describe_fd(
	const gchar *const  *prefix,
	gint                 fd,
	guint                timeout_ms,
	GCancellable        *cancellable,
	AxisMediaInfo      **info,
	gchar              **error
);

G_END_DECLS

#endif /* AXIS_PROBE_DESCRIBE_H */
