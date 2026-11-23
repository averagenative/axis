/*
 * axis-web-profiles.h - the conversion profile pages
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Register axis_web_profiles_get_module() after the estimate module (the
 * figures are its), passing the AxisWebProfiles as its user data.  Pages
 * (every logged-in user may look at a profile; every change, and the forms
 * that make one, are an administrator's, and each POST needs the CSRF
 * token; every change writes an audit line):
 *
 *   GET  /profiles                     every profile: kind, lock, where
 *                                      assigned, estimated saving
 *   GET  /profiles/:slug               one: the definition in words, its
 *                                      revisions, its Savings and
 *                                      Compatibility figures
 *   POST /profiles/:slug/copy          a new profile with its definition
 *   GET  /profiles/:slug/edit          the form (not for a built-in)
 *   POST /profiles/:slug/edit          action=preview: the effect on the
 *                                      eligibility counts and on the lock;
 *                                      action=save: a new revision, or
 *                                      nothing when nothing changed
 *   POST /profiles/:slug/archive       hidden from choices, record kept
 *   POST /profiles/:slug/restore
 *   GET  /profiles/assign              per root: its video and audio
 *                                      profile; with root and profile, the
 *                                      saving and verdict of that pairing
 *   POST /profiles/assign              root, profile, confirm (needed when
 *                                      the verdict is risky or unknown,
 *                                      worked out again here)
 *   POST /profiles/unassign            root, kind
 *
 * A profile's name and note are a person's text: they are shown as text
 * content only, never in a link or an attribute (links carry slugs, which
 * are fixed characters).  Assigning records intent: it creates no job and
 * opens no file.
 */

#ifndef AXIS_WEB_PROFILES_H
#define AXIS_WEB_PROFILES_H

#include <glib.h>

#include "compat/axis-compat.h"
#include "core/axis-module.h"
#include "transcode/axis-estimator.h"

G_BEGIN_DECLS

#define AXIS_PROFILES_MODULE_ID "profiles"

typedef struct _AxisWebProfiles AxisWebProfiles;

/* Create on the main thread; @estimator is referenced. */
AxisWebProfiles *
axis_web_profiles_new(AxisEstimator *estimator);

/* The compatibility report, for the verdicts (nullable: none shown). */
void
axis_web_profiles_set_compat(
	AxisWebProfiles *self,
	AxisCompat      *compat
);

/* Waits (running the default main context) for the page work in flight. */
void
axis_web_profiles_stop(AxisWebProfiles *self);

void
axis_web_profiles_free(AxisWebProfiles *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisWebProfiles, axis_web_profiles_free)

const AxisModuleInfo *
axis_web_profiles_get_module(void);

G_END_DECLS

#endif /* AXIS_WEB_PROFILES_H */
