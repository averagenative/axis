/*
 * axis-portal-log.c - so many log lines of one kind in a minute, and then
 * one line that says how many more there were
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "axis-portal-log.h"

#define MINUTE_US (G_GINT64_CONSTANT(60) * G_USEC_PER_SEC)

gboolean
axis_portal_log_budget_take(
	AxisPortalLogBudget *self,
	gint64               now_us
){
	g_return_val_if_fail(self != NULL, TRUE);

	if ((self->written == 0 && self->left_out == 0) || now_us - self->start_us >= MINUTE_US ||
	    now_us < self->start_us) {
		self->start_us = now_us;
		self->written = 0;
		self->left_out = 0;
	}
	if (self->written < AXIS_PORTAL_LOG_LINES_A_MINUTE) {
		self->written++;
		return TRUE;
	}
	if (self->left_out < G_MAXUINT)
		self->left_out++;
	return FALSE;
}

guint
axis_portal_log_budget_due(
	AxisPortalLogBudget *self,
	gint64               now_us
){
	guint due;

	g_return_val_if_fail(self != NULL, 0);

	if (self->left_out == 0)
		return 0;
	if (now_us != G_MAXINT64 && now_us - self->start_us < MINUTE_US && now_us >= self->start_us)
		return 0;
	due = self->left_out;
	self->written = 0;
	self->left_out = 0;
	return due;
}

gint64
axis_portal_log_budget_ends(const AxisPortalLogBudget *self)
{
	g_return_val_if_fail(self != NULL, G_MAXINT64);

	return self->left_out > 0 ? self->start_us + MINUTE_US : G_MAXINT64;
}
