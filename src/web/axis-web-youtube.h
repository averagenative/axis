/*
 * axis-web-youtube.h - the YouTube module: its pages and its job
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Media, then YouTube (/youtube): the videos Axis has, each with its state,
 * and, for administrators, the box an address is pasted into: Axis looks the
 * video up (what it is, its channel, its length, what would be taken), and
 * "Download" queues it.  System, then YouTube downloader (/admin/youtube):
 * the switch, with its notice, and which yt-dlp runs.
 *
 * Looking a video up runs yt-dlp, so it is handed to a worker thread; the
 * main loop only draws.  Every route that changes anything is for
 * administrators.
 */

#ifndef AXIS_WEB_YOUTUBE_H
#define AXIS_WEB_YOUTUBE_H

#include <glib.h>

#include "core/axis-module.h"
#include "storage/axis-storage.h"
#include "youtube/axis-youtube.h"

G_BEGIN_DECLS

typedef struct _AxisWebYoutube AxisWebYoutube;

/* @youtube and @storage must outlive the result. */
AxisWebYoutube *
axis_web_youtube_new(
	AxisYoutube *youtube,
	AxisStorage *storage
);

/* At the start of the daemon, once the job queue runs (so that it has taken
 * back the jobs of the run before): what a daemon that was killed during a
 * download left is put right on a worker thread (axis_youtube_recover()).
 * Main loop only. */
void
axis_web_youtube_recover(AxisWebYoutube *self);

/* Cancels a look in flight and a download that runs (axis_youtube_stop()),
 * then waits for the work in flight, which ends at once for that (main loop
 * only); nothing new starts. */
void
axis_web_youtube_stop(AxisWebYoutube *self);

void
axis_web_youtube_free(AxisWebYoutube *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisWebYoutube, axis_web_youtube_free)

/* The module; its user data is the AxisWebYoutube. */
const AxisModuleInfo *
axis_web_youtube_get_module(void);

G_END_DECLS

#endif /* AXIS_WEB_YOUTUBE_H */
