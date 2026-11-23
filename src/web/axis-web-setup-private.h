/*
 * axis-web-setup-private.h - shared by the files of the setup guide
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 *   axis-web-setup.c         the module: work off the main loop, the record
 *                            of choices, what each layer's check says
 *   axis-web-setup-pages.c   the overview and the steps' pages
 *   axis-web-setup-guides.c  the short guides: the words about what is
 *                            outside Axis (keys, categories, disks, paths)
 */

#ifndef AXIS_WEB_SETUP_PRIVATE_H
#define AXIS_WEB_SETUP_PRIVATE_H

#include <htmx-glib.h>

#include "compat/axis-compat-report.h"
#include "web/axis-web.h"
#include "web/axis-web-setup.h"

G_BEGIN_DECLS

/* Tasks waiting for a thread beyond this are answered 503. */
#define AXIS_WEB_SETUP_MAX_PENDING 8

/* The folders of a client's downloads looked at, at most. */
#define AXIS_WEB_SETUP_MAX_FOLDERS 4

/* What a token check looks up: a film every provider knows.  The one real
 * title in the code, on purpose: the check asks the real service, so a
 * made-up title would find nothing.  Name it only here (the tests use this
 * name, never the title). */
#define AXIS_WEB_SETUP_CHECK_TITLE "The Matrix"
#define AXIS_WEB_SETUP_CHECK_YEAR  1999

struct _AxisWebSetup
{
	AxisSettings     *settings;	/* owned references; the nullable ones may be NULL */
	AxisMeta         *meta;
	AxisIndexers     *indexers;
	AxisDownloads    *downloads;
	AxisStorage      *storage;
	AxisQualityStore *quality;
	AxisCompat       *compat;
	AxisIdentify     *identify;
	AxisWeb          *web;	/* set when the routes are registered */
	guint             pending;	/* (main loop) dispatched, not yet answered */
	gboolean          stopping;	/* (main loop) */
};

/**
 * AxisSetupState:
 * @done: the layer's own check passes
 * @skipped: put off ("Not now"), and not done
 * @dismissed: not used ("I don't use this"), and not done
 * @off: the module behind it is switched off (or not part of this Axis)
 * @summary: one sentence: what the check found, in plain words
 */
typedef struct
{
	gboolean  done;
	gboolean  skipped;
	gboolean  dismissed;
	gboolean  off;
	gchar    *summary;
} AxisSetupState;

/**
 * AxisSetupFolder:
 *
 * A folder of a download client's: one it has a mapping for, or one its
 * downloads were reported in (their parent folder), and where it is for
 * Axis.
 */
typedef struct
{
	gint64   client_id;
	gchar   *path;		/* the client's own path */
	gboolean reported;	/* the client named it (else: a mapping's prefix) */
	gboolean mapped;
	gint64   volume_id;	/* when mapped */
	gchar   *rel_path;	/* when mapped: the path on the volume */
	gchar   *problem;	/* when not mapped: why, a fragment */
} AxisSetupFolder;

/**
 * AxisSetupFigures:
 *
 * Everything the guide's pages show, read on a worker thread.  The lists
 * are %NULL when the layer they come from is not there.
 */
typedef struct
{
	gboolean            films;		/* the Movies module is on */
	gboolean            series;		/* the TV module is on */
	gboolean            meta_on;
	gboolean            indexers_on;
	gboolean            downloads_on;
	gboolean            storage_on;
	gboolean            quality_on;
	gboolean            compat_on;
	gboolean            identify_on;
	AxisReadiness       ready;
	GArray             *providers;	/* AxisMetaProviderStatus */
	GPtrArray          *sources;	/* AxisIndexerSourceInfo */
	GPtrArray          *clients;	/* AxisDownloadClientInfo */
	GPtrArray          *folders;	/* AxisSetupFolder */
	GPtrArray          *warnings;	/* AxisImportVolumeWarning */
	GPtrArray          *volumes;	/* AxisVolumeInfo */
	GPtrArray          *roots;	/* AxisRootInfo */
	GPtrArray          *profiles;	/* AxisQualityStored */
	gchar              *default_slug;
	gboolean            default_chosen;	/* stored, not the starting value */
	AxisCompatCoverage  coverage;
	gboolean            coverage_read;
	GPtrArray          *identify_roots;	/* AxisIdentifyRootInfo */
	gint64              recycle_s;	/* how long replaced files are kept; -1: not known */
	gboolean            read_only;
	guint               skipped;	/* AXIS_SETUP_BIT() */
	guint               dismissed;
	AxisSetupState      states[AXIS_SETUP_N_STEPS];
	/* A check run by this request (POST .../check). */
	gboolean            checked;
	gboolean            check_ok;
	gchar              *check_text;
	GError             *error;
} AxisSetupFigures;

void
axis_setup_figures_free(AxisSetupFigures *figures);

/* The first step to carry on with: the first that is not done, dismissed
 * or skipped, then the first skipped; -1 when every step not dismissed is
 * done. */
gint
axis_setup_figures_resume(const AxisSetupFigures *figures);

/* The step after @step to go to: the next not done and not dismissed, or
 * -1 for none (the overview). */
gint
axis_setup_figures_next(
	const AxisSetupFigures *figures,
	AxisSetupStep           step
);

/* The registry the shell serves (main loop). */
AxisModuleRegistry *
axis_web_setup_registry(AxisWebSetup *self);

/* What a source's last contact found, in a sentence: "Prowlarr answered and
 * reports 12 indexers, 3 of them disabled." */
gchar *
axis_web_setup_source_sentence(const AxisIndexerSourceInfo *source);

/* The same of a client ("SABnzbd refused the API key: check it below."). */
gchar *
axis_web_setup_client_sentence(
	const AxisDownloadClientInfo *client,
	const AxisDownloadKindInfo   *kind
);

/* ── Pages (axis-web-setup-pages.c) ─────────────────────────────── */

HtmxResponse *
axis_web_setup_overview_page(
	AxisWebSetup     *self,
	HtmxRequest      *request,
	AxisSetupFigures *figures,
	const gchar      *done
);

HtmxResponse *
axis_web_setup_step_page(
	AxisWebSetup     *self,
	HtmxRequest      *request,
	AxisSetupFigures *figures,
	AxisSetupStep     step,
	const gchar      *done
);

/* ── Pieces (axis-web-setup-guides.c) ──────────────────────────── */

/* Adds @child to @parent and drops the caller's reference. */
void
axis_web_setup_add(
	gpointer parent,
	gpointer child
);

/* A paragraph of @text, with @css (nullable). */
HtmxNode *
axis_web_setup_text(
	const gchar *css,
	const gchar *text
);

/* A link out of Axis: a new tab, no referrer, no opener. */
HtmxNode *
axis_web_setup_link_out(
	const gchar *href,
	const gchar *text
);

/*
 * A link to a page of a service an administrator configured: its
 * configured @address (an http or https origin) and @base_path (as stored:
 * "" or "/segments"), then @fixed_path, a constant of Axis's.  %NULL when
 * the address is not such an origin.
 */
HtmxNode *
axis_web_setup_link_to_service(
	const gchar *address,
	const gchar *base_path,
	const gchar *fixed_path,
	const gchar *text
);

/* A closed <details class="guide-more"> with @summary, to add the guide to. */
HtmxNode *
axis_web_setup_more(const gchar *summary);

/* The guides of task 1.5, each a guide-more block. */
HtmxNode *
axis_web_setup_guide_category(void);

HtmxNode *
axis_web_setup_guide_one_disk(void);

HtmxNode *
axis_web_setup_guide_mapping(void);

HtmxNode *
axis_web_setup_guide_token(void);

/* What writing means: what Axis does, and what it never does
 * (@recycle_s: how long replaced files are kept, -1 when not known). */
HtmxNode *
axis_web_setup_guide_writing(gint64 recycle_s);

/* "7 days", "12 hours", "30 minutes". */
gchar *
axis_web_setup_duration_text(gint64 seconds);

G_END_DECLS

#endif /* AXIS_WEB_SETUP_PRIVATE_H */
