/*
 * axis-estimator-private.h - shared by the files of the estimator object
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef AXIS_ESTIMATOR_PRIVATE_H
#define AXIS_ESTIMATOR_PRIVATE_H

#include "transcode/axis-estimator.h"
#include "transcode/axis-sample-args.h"

G_BEGIN_DECLS

struct _AxisEstimator
{
	gint               ref_count;	/* atomic */
	gint               enabled;	/* atomic */
	AxisDbPool        *pool;
	AxisStorage       *storage;	/* owned reference */
	AxisSettings      *settings;	/* nullable, owned reference */

	GMutex             lock;
	AxisEstimateModel *model;	/* (lock) shared, reference counted */
	gint64             model_sample;	/* (lock) the newest sample in it */
	gboolean           model_ready;	/* (lock) */
	AxisSampleTuning   tuning;	/* (lock) */
	gchar            **tool_command;	/* (lock) a copy of tuning.tool_command */
	gchar             *scratch_parent;	/* (lock) a copy */
};

/* The settings as the targets need them. */
void
axis_estimator_get_config(
	AxisEstimator      *self,
	AxisEstimateConfig *config
);

/* A string that changes when what shapes the scheduled figures does: the
 * model, the setting, and the revisions refreshed on schedule
 * (@scheduled, AxisProfileRevision). */
gchar *
axis_estimator_config_key(
	AxisEstimator *self,
	GPtrArray     *scheduled
);

/* The current revisions of the profiles refreshed on schedule (built-in,
 * or assigned to a root), and with @requested those an administrator asked
 * figures for, built-ins first (AxisProfileRevision). */
gboolean
axis_estimator_scheduled(
	AxisDbConn  *conn,
	gboolean     requested,
	GPtrArray  **revisions,
	GError     **error
);

/* The configured target of each usable revision of @revisions (a revision
 * whose definition is not usable is left out, and said so in the log). */
GArray *
axis_estimator_targets_of(
	AxisEstimator *self,
	GPtrArray     *revisions
);

gint64
axis_estimator_get_integer(
	AxisEstimator *self,
	const gchar   *key,
	gint64         fallback
);

gchar *
axis_estimator_get_string(
	AxisEstimator *self,
	const gchar   *key,
	const gchar   *fallback
);

/* Evaluates every file of @rows for @target, each audio file with its
 * album (loaded through @conn when not all of it is in @rows): @results
 * gets one result per file of @rows, in order. */
gboolean
axis_estimator_evaluate_rows(
	AxisDbConn               *conn,
	AxisEstimateRows         *rows,
	const AxisEstimateTarget *target,
	const AxisEstimateModel  *model,
	AxisEstimateResult       *results,
	GError                  **error
);

/* Reads the calibration samples into a finished model, and the newest
 * sample's id; in the caller's transaction, or in one of its own. */
gboolean
axis_estimator_read_model(
	AxisDbConn         *conn,
	gboolean            in_transaction,
	AxisEstimateModel **model,
	gint64             *newest,
	GError            **error
);

/* axis_estimate_sample_argv() is in transcode/axis-sample-args.h, shared
 * with axis-worker. */

/* The SQL condition (over f, r and m) that every file @outcome can match
 * meets for @target: a superset, never the decision. */
gchar *
axis_estimator_list_narrowing(
	const AxisEstimateTarget *target,
	const gchar              *outcome
);

/* One statement in a transaction of its own under @timeout. */
gboolean
axis_estimator_timed_exec(
	AxisDbConn          *conn,
	const gchar         *timeout,
	const gchar         *sql,
	guint                n_params,
	const gchar *const  *params,
	AxisDbResult       **result,
	GError             **error
);

G_END_DECLS

#endif /* AXIS_ESTIMATOR_PRIVATE_H */
