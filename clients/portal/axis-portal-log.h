/*
 * axis-portal-log.h - so many log lines of one kind in a minute, and then
 * one line that says how many more there were
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * (add-request-portal, P3 after its review.)  A refusal costs a stranger
 * nothing, and each one used to be a line of the log: a sender who is
 * refused two thousand times a second wrote gigabytes a day to the host that
 * runs the daemon.  Each kind of line now has a budget: the first
 * AXIS_PORTAL_LOG_LINES_A_MINUTE of a minute are written, the rest are
 * counted, and the count is written as one line when the minute has ended.
 * The caller counts what it refused itself; this file counts lines only.
 *
 * Time is passed in (monotonic microseconds).  Not thread-safe.
 */

#ifndef AXIS_PORTAL_LOG_H
#define AXIS_PORTAL_LOG_H

#include <glib.h>

G_BEGIN_DECLS

#define AXIS_PORTAL_LOG_LINES_A_MINUTE 10

typedef struct
{
	gint64 start_us;	/* when its minute began */
	guint  written;		/* lines written in that minute */
	guint  left_out;	/* lines counted instead */
} AxisPortalLogBudget;

/* Whether a line of this kind is written at @now_us (%TRUE) or only
 * counted.  Say what axis_portal_log_budget_due() has to say first. */
gboolean
axis_portal_log_budget_take(
	AxisPortalLogBudget *self,
	gint64               now_us
);

/* How many lines were left out of a minute that has ended by @now_us (of
 * any minute, with %G_MAXINT64), to be said in one line; the count starts
 * again.  0: there is nothing to say. */
guint
axis_portal_log_budget_due(
	AxisPortalLogBudget *self,
	gint64               now_us
);

/* When the minute that has lines left out ends; %G_MAXINT64 when none are. */
gint64
axis_portal_log_budget_ends(const AxisPortalLogBudget *self);

G_END_DECLS

#endif /* AXIS_PORTAL_LOG_H */
