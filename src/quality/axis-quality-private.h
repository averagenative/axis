/*
 * axis-quality-private.h - shared by the files of the quality profiles
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef AXIS_QUALITY_PRIVATE_H
#define AXIS_QUALITY_PRIVATE_H

#include "quality/axis-quality.h"

G_BEGIN_DECLS

/* Appends a problem (@problems nullable). */
void
axis_quality_problem_add(
	GPtrArray   *problems,
	const gchar *field,
	const gchar *message
);

/* What @release has for @condition, in words ("H.264 (x264)", "the group
 * EVO"), or %NULL when the name states nothing of it. */
gchar *
axis_quality_rule_has_text(
	AxisQualityCondition  condition,
	const AxisRelease    *release
);

/* The values of @rule that hold for @release, in words, or %NULL. */
gchar *
axis_quality_rule_matched_text(
	const AxisQualityRule *rule,
	const AxisRelease     *release,
	const gchar           *name
);

/* The rule's values in words, joined by "or" ("HEVC (x265) or AV1"). */
gchar *
axis_quality_rule_values_text(const AxisQualityRule *rule);

/* The values sorted as the canonical form has them (in place). */
void
axis_quality_rule_sort_values(AxisQualityRule *rule);

/* Whether two values of @condition are the same value (ignoring case for
 * groups and words). */
gboolean
axis_quality_value_equal(
	AxisQualityCondition  condition,
	const gchar          *a,
	const gchar          *b
);

/* The revision rank of what was read: 0 the original, 1 a proper, repack
 * or rerip (or v2), 2 for a second (REAL with one, v3), ... */
gint
axis_quality_revision_rank(const AxisRelease *release);

/* A list joined as "a, b and c" (or "or"). */
gchar *
axis_quality_join(
	const GPtrArray *items,
	const gchar     *last
);

G_END_DECLS

#endif /* AXIS_QUALITY_PRIVATE_H */
