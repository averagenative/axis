/*
 * axis-conversion-run-private.h - shared by the files of the conversion runs
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef AXIS_CONVERSION_RUN_PRIVATE_H
#define AXIS_CONVERSION_RUN_PRIVATE_H

#include "transcode/axis-conversion-run.h"
#include "transcode/axis-conversion-private.h"

G_BEGIN_DECLS

struct _AxisConversionRuns
{
	gint             refs;	/* atomic */
	AxisConversions *conversions;
	GMutex           lock;
	gint64           now_us;	/* (lock) tests; 0: the clock */
};

/* Now, real time (the tests' clock when set). */
gint64
axis_conversion_run_now(AxisConversionRuns *self);

/* The last unit within the caps of @run_id (-1: every unit), worked out
 * from its units (not removed) in order; in the caller's connection. */
gboolean
axis_conversion_run_cap_unit(
	AxisDbConn  *conn,
	gint64       run_id,
	gint64       cap_files,
	gint64       cap_bytes,
	gint        *cap_unit,
	GError     **error
);

/* The text of a run's revision for a sentence ("Space saver, revision 2"). */
gchar *
axis_conversion_run_profile_text(
	const gchar *name,
	gint         number
);

/* The minutes after local midnight of @us. */
gint
axis_conversion_run_minute_of_day(gint64 us);

/* Whether @minute is inside the window [@start, @end) (it may wrap past
 * midnight); always when @start is -1. */
gboolean
axis_conversion_run_in_window(
	gint start,
	gint end,
	gint minute
);

G_END_DECLS

#endif /* AXIS_CONVERSION_RUN_PRIVATE_H */
