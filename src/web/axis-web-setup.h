/*
 * axis-web-setup.h - the setup guide: a path, step by step, through what Axis
 * needs before it can find, download and import a title (System, Setup
 * guide; administrators only)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 *   GET  /setup-guide                  the steps, what each has found, and
 *                                      where to carry on
 *   GET  /setup-guide/:step            one step: what the thing is and why
 *                                      Axis needs it, where its key or
 *                                      address is found, the real form of
 *                                      the page it belongs to, and what the
 *                                      layer's own check says
 *   POST /setup-guide/:step/check      the layer's own test, now (TMDb's
 *                                      token by a lookup, a media server's
 *                                      connection), answered on the page
 *   POST /setup-guide/:step/skip       "Not now": the guide moves on, and
 *                                      comes back to it last
 *   POST /setup-guide/:step/dismiss    "I don't use this": the guide and the
 *                                      dashboard stop asking for it
 *   POST /setup-guide/:step/restore    ask for it again
 *
 * The guide is a path, not a second configuration.  Each step shows the
 * form of the page the thing is configured on, built by that page's own
 * function, posting to that page's own handler, which comes back to the
 * step (web/axis-web-setup-path.h); there is one value of everything and
 * nothing to keep in step.  A step is done only when its layer's own check
 * says so (a source that answered, a client that answered with a mapped
 * folder, a root on a volume that is online, ...), never because it was
 * visited.  Skipped and dismissed steps are the installation's record
 * (0030_setup_guide.sql), the same in every browser.
 *
 * No secret is ever put in a page: a key's field is empty and says whether
 * one is stored.  Outbound links open in a new tab without a referrer; a
 * link to a service an administrator configured is built from its
 * configured address and base path and a fixed path of Axis's, never from
 * anything the service said.  Nothing here contacts a service on a GET.
 */

#ifndef AXIS_WEB_SETUP_H
#define AXIS_WEB_SETUP_H

#include <glib.h>

#include "compat/axis-compat.h"
#include "core/axis-module.h"
#include "core/axis-settings.h"
#include "download/axis-download.h"
#include "identify/axis-identify.h"
#include "indexer/axis-indexer.h"
#include "meta/axis-meta.h"
#include "quality/axis-quality-store.h"
#include "storage/axis-storage.h"
#include "web/axis-web-setup-path.h"

G_BEGIN_DECLS

#define AXIS_WEB_SETUP_MODULE_ID "setup-guide"

typedef struct _AxisWebSetup AxisWebSetup;

/**
 * AxisWebSetupLayers:
 * @settings: the store (the keys, the default profile, storage.read_only)
 * @meta: (nullable): titles and artwork
 * @indexers, @downloads, @storage: the acquisition layers
 * @quality: (nullable): the quality profiles
 * @compat: (nullable): the media servers
 * @identify: (nullable): identification of the films roots
 *
 * What the guide asks.  Each is referenced.
 */
typedef struct
{
	AxisSettings     *settings;
	AxisMeta         *meta;
	AxisIndexers     *indexers;
	AxisDownloads    *downloads;
	AxisStorage      *storage;
	AxisQualityStore *quality;
	AxisCompat       *compat;
	AxisIdentify     *identify;
} AxisWebSetupLayers;

AxisWebSetup *
axis_web_setup_new(const AxisWebSetupLayers *layers);

/* Waits for the page work in flight (main loop). */
void
axis_web_setup_stop(AxisWebSetup *self);

void
axis_web_setup_free(AxisWebSetup *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisWebSetup, axis_web_setup_free)

/* A required module, "Setup guide", with the pages above and System's entry. */
const AxisModuleInfo *
axis_web_setup_get_module(void);

/**
 * axis_web_setup_load_choices:
 * @skipped: (out): AXIS_SETUP_BIT() of each step put off ("Not now")
 * @dismissed: (out): of each step not used ("I don't use this")
 *
 * The installation's choices.  Blocks on the database: a worker thread.
 *
 * Returns: %TRUE on success
 */
gboolean
axis_web_setup_load_choices(
	AxisDbPool  *pool,
	guint       *skipped,
	guint       *dismissed,
	GError     **error
);

G_END_DECLS

#endif /* AXIS_WEB_SETUP_H */
