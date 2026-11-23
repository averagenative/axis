/*
 * axis-discover-private.h - shared by the files of the Discover layer
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef AXIS_DISCOVER_PRIVATE_H
#define AXIS_DISCOVER_PRIVATE_H

#include "discover/axis-discover.h"

#include "db/axis-db-conn.h"
#include "db/axis-db-result.h"

G_BEGIN_DECLS

struct _AxisDiscover
{
	gint          refs;		/* atomic */
	AxisDbPool   *pool;
	AxisSettings *settings;		/* owned reference, nullable */
	AxisMeta     *meta;		/* owned reference */
};

/* "(extract(epoch FROM column) * 1000000)::bigint" */
#define AXIS_DISCOVER_EPOCH_US(column) "(extract(epoch FROM " column ") * 1000000)::bigint"

/* The provider that can be asked for @seed's recommendations: the one
 * Discover uses for its kind, when it offers recommendations and @seed has
 * an id of its (the record is the provider's own, or keeps the provider's id
 * among its identifiers); else %NULL.  @ready: (out) (optional) as
 * axis_discover_provider().  @ref: (out) (optional) (transfer none): that
 * id, @seed's own. */
const AxisMetaProviderInfo *
axis_discover_like_provider(
	AxisDiscover         *self,
	const AxisMetaTitle  *seed,
	gboolean             *ready,
	const gchar         **ref
);

/* A whole number as the text of a parameter. */
gchar *
axis_discover_int_text(gint64 value);

/* The text form of a PostgreSQL bigint[] of @values (gint64). */
gchar *
axis_discover_sql_int64_array(GArray *values);

/* The text form of an integer[] of @values (gint); with @null_below set, a
 * value below it is NULL. */
gchar *
axis_discover_sql_int_array(
	GArray   *values,
	gboolean  with_nulls,
	gint      null_below
);

/* The text form of a text[] of @values (each quoted). */
gchar *
axis_discover_sql_text_array(GPtrArray *values);

G_END_DECLS

#endif /* AXIS_DISCOVER_PRIVATE_H */
