/*
 * axis-web-manual.h - a manual search's pages, for every module that
 * searches: asking for one, what it found (grouped, sorted, filtered),
 * Grab this one with its confirmation, and the audit
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A module registers its levels (a movie; a series, a season, an episode;
 * later an album, a track) and gets these routes under its prefix:
 *
 *   GET  <prefix>/:id/manual?level=&item=      the level's page, open to every
 *                                              logged-in user (&part=results:
 *                                              its results card alone, which
 *                                              htmx polls while a search runs;
 *                                              &sort=, &indexer=, &protocol=)
 *   POST <prefix>/:id/manual                   ask for one (level, item)
 *   GET  <prefix>/:id/manual/grab?candidate=   the confirmation (and level,
 *                                              item)
 *   POST <prefix>/:id/manual/grab              Grab this one (candidate,
 *                                              confirm=yes, downgrade=yes,
 *                                              numbering=yes, level, item, and
 *                                              replace_<library file id>=yes for
 *                                              each file kept by hand it replaces)
 *   GET  <prefix>/:id/manual/force?candidate=  the confirmation of a grab by
 *                                              force (and level, item; with
 *                                              picked=yes, pick_<id>=yes and
 *                                              whole=<n>: what the person names
 *                                              in the picker, for a series)
 *   POST <prefix>/:id/manual/force             Grab it by force (candidate,
 *                                              confirm=yes, digest, checked=yes,
 *                                              replace_<library file id>=yes for
 *                                              each file ticked, level, item,
 *                                              and pick_<id>=yes or whole=<n>)
 *   POST <prefix>/:id/manual/block             Block this release (candidate,
 *                                              level, item): it goes on the
 *                                              owner's blocklist by the user,
 *                                              and the page comes back at its
 *                                              row (add-block-release)
 *
 * A release that a fixed rule about its name rules out (not the title, more
 * than one title fits, no title readable) is offered "Grab it by force" to
 * an administrator, when the module answers force_preview() and
 * force_grab(); the ordinary confirmation and grab keep refusing it.
 *
 * Asking and grabbing are an administrator's (the route and the CSRF check);
 * the confirmation is an administrator's page too.  Every grab is in the
 * audit with the user and "by hand".  The module's callbacks run off the
 * main loop, counted by its hold() and release() so that its stop waits
 * for them; the registration is kept by the shell.
 */

#ifndef AXIS_WEB_MANUAL_H
#define AXIS_WEB_MANUAL_H

#include <glib.h>
#include <htmx-glib.h>

#include "acquire/axis-acquire.h"
#include "web/axis-web.h"

G_BEGIN_DECLS

/**
 * AxisWebManualLevel:
 * @name: the level's word in the routes ("season")
 * @item: it names an item (?item=: a season's number, an episode's id)
 */
typedef struct
{
	const gchar *name;
	gboolean     item;
} AxisWebManualLevel;

/* What one page is for.  For a grab by force of a module whose owner has
 * parts (a series): @named when the person named what the release is for (the
 * confirmation's picker, picked=yes, or its post), then @picked (gint64: the
 * items ticked, pick_<id>=yes) or @whole with @part (a whole part, whole=<n>:
 * a season). */
typedef struct
{
	gint64                    owner_id;
	const AxisWebManualLevel *level;
	gint64                    item;
	gboolean                  named;
	const GArray             *picked;
	gboolean                  whole;
	gint64                    part;
} AxisWebManualTarget;

/**
 * AxisWebManualRead:
 * @title: the owner's label ("Lantern Bay (2025)")
 * @what: what the level is, in a sentence's words ("season 1", "S01E03,
 *   Third Bell", "the whole series", "the movie")
 * @back: the page it belongs to (a path on this site)
 * @automatic: (nullable): the page of its last automatic search's releases
 * @search: (nullable): the level's last manual search
 * @candidates: (element-type AxisAcquireCandidate): its releases
 * @holds: (element-type utf8) (nullable): what each holds ("S01E02"), one
 *   per candidate (entries nullable), for a module whose releases hold parts
 * @running: a manual search of the level is queued or running
 * @state: (nullable): where it stands, to follow "A manual search of ... is "
 *   (axis_web_activity_state())
 * @blocklist: (nullable): where the owner's blocklist is listed (a path on
 *   this site, with its anchor), for a row that is on it
 *
 * What a module reads for the page.
 */
typedef struct
{
	gchar                 *title;
	gchar                 *what;
	gchar                 *back;
	gchar                 *automatic;
	AxisAcquireSearchInfo *search;
	GPtrArray             *candidates;
	GPtrArray             *holds;
	gboolean               running;
	gchar                 *state;
	gchar                 *blocklist;
} AxisWebManualRead;

void
axis_web_manual_read_clear(AxisWebManualRead *read);

/**
 * AxisWebManualInfo:
 * @prefix: the module's routes' prefix ("/series", "/films")
 * @noun: what an owner is ("series", "movie"), for "There is no such ..."
 * @levels: ended by an entry with a %NULL name
 * @hold: (main loop) %FALSE when the module stops or is busy; else one more
 *   piece of work of its is in flight
 * @release: (main loop) that work is done
 * @status: the HTTP status of one of the module's errors (0: an internal
 *   error)
 * @read: (worker thread) the page's facts; an error for no such owner or
 *   item
 * @search: (worker thread) queue a manual search of the level (*@created
 *   %FALSE: nothing new: one of it or of a wider level was queued or running
 *   already, or one ended a moment ago, %AXIS_ACQUIRE_SEARCH_AGAIN_S)
 * @preview: (worker thread) what a grab by hand would do
 *   (axis_acquire_hand_preview())
 * @grab: (worker thread) Grab this one (axis_acquire_grab_by_hand()) with
 *   the ticks, *@grabbed what was recorded; *@result says what came of it
 *   (CHANGED: a replace of the download in flight was asked for and what
 *   stands in the way is not what the page showed: the page is shown again),
 *   the error in the module's words when nothing was sent
 * @force_preview: (nullable) (worker thread) what a grab by force would do
 *   (axis_acquire_force_preview()); without it and @force_grab the module's
 *   releases are not offered "Grab it by force", and the routes answer 404
 * @force_grab: (nullable) (worker thread) Grab it by force
 *   (axis_acquire_grab_by_force()); *@result says what came of it (CHANGED:
 *   the page is shown again), the error in the module's words when nothing
 *   was sent
 * @block: (nullable) (worker thread) Block this release
 *   (axis_acquire_block_candidate()); without it no row offers it, and the
 *   route answers 404
 */
typedef struct
{
	const gchar              *prefix;
	const gchar              *noun;
	const AxisWebManualLevel *levels;
	gboolean (*hold)(gpointer data);
	void     (*release)(gpointer data);
	guint    (*status)(const GError *error);
	gboolean (*read)(gpointer data, const AxisWebManualTarget *target, AxisWebManualRead *read, GError **error);
	gboolean (*search)(gpointer data, const AxisWebManualTarget *target, const gchar *user, gboolean *created,
	                   GError **error);
	gboolean (*preview)(gpointer data, const AxisWebManualTarget *target, gint64 candidate_id,
	                    AxisAcquireHandPreview **preview, GError **error);
	gboolean (*grab)(gpointer data, const AxisWebManualTarget *target, gint64 candidate_id,
	                 const AxisAcquireHandTicks *ticks, const gchar *user, AxisAcquireHandGrabbed *grabbed,
	                 AxisAcquireGrabResult *result, GError **error);
	gboolean (*force_preview)(gpointer data, const AxisWebManualTarget *target, gint64 candidate_id,
	                          AxisAcquireHandPreview **preview, GError **error);
	gboolean (*force_grab)(gpointer data, const AxisWebManualTarget *target, gint64 candidate_id,
	                       const AxisAcquireForceTicks *ticks, const gchar *user, AxisAcquireHandGrabbed *grabbed,
	                       AxisAcquireGrabResult *result, GError **error);
	gboolean (*block)(gpointer data, const AxisWebManualTarget *target, gint64 candidate_id, const gchar *user,
	                  GError **error);
} AxisWebManualInfo;

/* Registers the routes (from the module's register_routes: they carry its
 * switch); @info must outlive the shell, @data is the module's. */
void
axis_web_manual_register(
	AxisWeb                 *web,
	const AxisWebManualInfo *info,
	gpointer                 data
);

/* "<prefix>/<id>/manual?level=<level>[&item=<item>]" (@item < 0: none). */
gchar *
axis_web_manual_path(
	const gchar *prefix,
	gint64       owner_id,
	const gchar *level,
	gint64       item
);

/* An administrator's button that asks for a manual search of a level (and
 * goes to its page). */
HtmxNode *
axis_web_manual_button(
	HtmxRequest *request,
	const gchar *prefix,
	gint64       owner_id,
	const gchar *level,
	gint64       item,
	const gchar *label
);

G_END_DECLS

#endif /* AXIS_WEB_MANUAL_H */
