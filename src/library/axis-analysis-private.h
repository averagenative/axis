/*
 * axis-analysis-private.h - shared by the files of the analysis
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef AXIS_ANALYSIS_PRIVATE_H
#define AXIS_ANALYSIS_PRIVATE_H

#include "library/axis-analysis.h"

G_BEGIN_DECLS

struct _AxisAnalysis
{
	gint                ref_count;	/* atomic */
	AxisDbPool         *pool;
	AxisSettings       *settings;	/* nullable, owned reference */
	GMutex              lock;
	AxisAnalysisTuning  tuning;	/* (lock) */
};

/* The defaults of the settings, as the code reads them without a store. */
#define AXIS_ANALYSIS_DEFAULT_LARGE_H264  0.30
#define AXIS_ANALYSIS_DEFAULT_LARGE_HEVC  0.15
#define AXIS_ANALYSIS_DEFAULT_LARGE_AV1   0.12
#define AXIS_ANALYSIS_DEFAULT_LARGE_VP9   0.15
#define AXIS_ANALYSIS_DEFAULT_MIN_INTERVAL (3600)
#define AXIS_ANALYSIS_DEFAULT_SETTLE      (600)
#define AXIS_ANALYSIS_DEFAULT_KEEP_DAILY  (400 * 24 * 3600)
#define AXIS_ANALYSIS_DEFAULT_KEEP_OTHER  (7 * 24 * 3600)

/* The thresholds in thousandths of a bit per pixel, per codec. */
void
axis_analysis_get_thresholds(
	AxisAnalysis *self,
	gint64        milli[AXIS_ANALYSIS_N_LARGE_CODECS]
);

gint64
axis_analysis_get_duration(
	AxisAnalysis *self,
	const gchar  *key,
	gint64        fallback
);

/* The refresh itself (axis-analysis-refresh.c). */
gboolean
axis_analysis_refresh_run(
	AxisAnalysis    *self,
	const gchar     *trigger,
	gint64           job_id,
	AxisJobContext  *context,
	gint64          *snapshot_id,
	GError         **error
);

G_END_DECLS

#endif /* AXIS_ANALYSIS_PRIVATE_H */
