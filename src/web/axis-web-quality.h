/*
 * axis-web-quality.h - the quality profile pages: Media management, Quality
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Register axis_web_quality_get_module() with the AxisWebQuality as its user
 * data.  Every logged-in user may look at the profiles and try a name; every
 * change, and the forms that make one, are an administrator's, each POST
 * needs the CSRF token, and every change writes an audit line.  No page here
 * makes an outbound request: trying a name reads it and decides it, nothing
 * is searched, grabbed or downloaded.
 *
 *   GET  /quality                 every profile: its qualities, cutoff, what
 *                                 uses it; the Try a name box
 *   GET  /quality/try             ?name=&profile=&runtime=&size=: what the
 *                                 parser read from the name and the decision
 *                                 with its reasons
 *   GET  /quality/:slug           one: the profile in words, its sizes and
 *                                 rules, what uses it
 *   POST /quality/:slug/copy      a new profile with its definition
 *   GET  /quality/:slug/edit      the form (a built-in: refused, with the copy
 *                                 offered)
 *   POST /quality/:slug/edit      op=save, or a change to the form only:
 *                                 up:Q, down:Q, join:Q, remove:Q, allow:Q
 *                                 (the order of the qualities, by buttons,
 *                                 without script), add_rule, remove_rule:N
 *   POST /quality/:slug/delete    when nothing uses it (never a built-in)
 *
 * A profile's name and a tried release name are a person's text: shown as
 * text content only, never in a link (links carry slugs); a value that goes
 * back into a form field is cut to the characters its field is made of.
 */

#ifndef AXIS_WEB_QUALITY_H
#define AXIS_WEB_QUALITY_H

#include <glib.h>

#include "compat/axis-compat.h"
#include "core/axis-module.h"
#include "quality/axis-quality-store.h"

G_BEGIN_DECLS

typedef struct _AxisWebQuality AxisWebQuality;

/* Create on the main thread; @store is referenced. */
AxisWebQuality *
axis_web_quality_new(AxisQualityStore *store);

/* The household's players, for the players preference when a name is tried
 * (nullable: none). */
void
axis_web_quality_set_compat(
	AxisWebQuality *self,
	AxisCompat     *compat
);

/* Waits (running the default main context) for the page work in flight. */
void
axis_web_quality_stop(AxisWebQuality *self);

void
axis_web_quality_free(AxisWebQuality *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisWebQuality, axis_web_quality_free)

const AxisModuleInfo *
axis_web_quality_get_module(void);

G_END_DECLS

#endif /* AXIS_WEB_QUALITY_H */
