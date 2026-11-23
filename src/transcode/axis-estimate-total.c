/*
 * axis-estimate-total.c - adding estimates up with honest ranges
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The middle of a total is the sum of the files' middles, so that a figure
 * is exactly what its files' pages add up to.  The range: a prior that is
 * wrong is wrong for every file it is used for, so within one correlation
 * group (axis_estimate_model_cell_info()) the files' deviations from their
 * middles add up in full; different groups are taken as independent and
 * combine as the square root of the sum of squares.  Before calibration
 * every cell of one codec and encoder family is one group, so the range of
 * a total is close to the sum of the files' ranges: wide, and honest.
 */

#include "transcode/axis-estimate-private.h"

#include <math.h>

typedef struct
{
	gdouble below;	/* the sum of mid - low */
	gdouble above;	/* the sum of high - mid */
} Deviation;

struct _AxisEstimateTotal
{
	gint64      files;
	gint64      now;
	gint64      mid;
	GHashTable *groups;	/* group -> Deviation */
	GArray     *cells;	/* guint, each once */
};

/*
 * Totals are sums of byte counts that come from the index and the probe,
 * and nothing promises that those are sane (a corrupt row could claim a
 * file of 2^63 - 1 bytes).  A sum that would not fit stays at the largest
 * value instead of overflowing.
 */
static gint64
saturating_add(
	gint64 a,
	gint64 b
){
	if (b > 0 && a > G_MAXINT64 - b)
		return G_MAXINT64;
	if (b < 0 && a < G_MININT64 - b)
		return G_MININT64;
	return a + b;
}

/* A non-negative double as whole bytes, within what a gint64 holds. */
static gint64
whole_bytes(gdouble value)
{
	if (!(value > 0))	/* also catches NaN */
		return 0;
	if (value >= 9.2e18)
		return G_MAXINT64;
	return (gint64)llround(value);
}

AxisEstimateTotal *
axis_estimate_total_new(void)
{
	AxisEstimateTotal *total = g_new0(AxisEstimateTotal, 1);

	total->groups = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
	total->cells = g_array_new(FALSE, FALSE, sizeof(guint));
	return total;
}

void
axis_estimate_total_free(AxisEstimateTotal *total)
{
	if (total == NULL)
		return;
	g_hash_table_unref(total->groups);
	g_array_unref(total->cells);
	g_free(total);
}

void
axis_estimate_total_add(
	AxisEstimateTotal        *total,
	const AxisEstimateModel  *model,
	const AxisEstimateResult *result
){
	AxisEstimateCellInfo info;
	Deviation *deviation;
	guint i;

	g_return_if_fail(total != NULL && model != NULL && result != NULL);

	total->files++;
	total->now = saturating_add(total->now, result->now);
	total->mid = saturating_add(total->mid, result->mid);

	axis_estimate_model_cell_info(model, result->cell, &info);
	deviation = g_hash_table_lookup(total->groups, GUINT_TO_POINTER(info.group + 1));
	if (deviation == NULL) {
		deviation = g_new0(Deviation, 1);
		g_hash_table_insert(total->groups, GUINT_TO_POINTER(info.group + 1), deviation);
	}
	deviation->below += (gdouble)(result->mid - result->low);
	deviation->above += (gdouble)(result->high - result->mid);

	for (i = 0; i < total->cells->len; i++) {
		if (g_array_index(total->cells, guint, i) == result->cell)
			return;
	}
	g_array_append_val(total->cells, result->cell);
}

void
axis_estimate_total_get(
	const AxisEstimateTotal *total,
	gint64                  *files,
	gint64                  *now,
	gint64                  *low,
	gint64                  *mid,
	gint64                  *high
){
	GHashTableIter iter;
	gpointer value;
	gdouble below = 0;
	gdouble above = 0;
	gint64 l;
	gint64 h;

	g_hash_table_iter_init(&iter, total->groups);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		const Deviation *deviation = value;

		below += deviation->below * deviation->below;
		above += deviation->above * deviation->above;
	}
	l = saturating_add(total->mid, -whole_bytes(sqrt(below)));
	h = saturating_add(total->mid, whole_bytes(sqrt(above)));
	/* Never below nothing, never above the files' size now (no file is
	 * estimated above its own size). */
	l = CLAMP(l, 0, total->mid);
	h = CLAMP(h, total->mid, MAX(total->now, total->mid));

	if (files != NULL)
		*files = total->files;
	if (now != NULL)
		*now = total->now;
	if (low != NULL)
		*low = l;
	if (mid != NULL)
		*mid = total->mid;
	if (high != NULL)
		*high = h;
}

const GArray *
axis_estimate_total_cells(const AxisEstimateTotal *total)
{
	return total->cells;
}
