/*
 * axis-web-storage-private.h - shared by the files of the storage pages
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef AXIS_WEB_STORAGE_PRIVATE_H
#define AXIS_WEB_STORAGE_PRIVATE_H

#include <htmx-glib.h>

#include "core/axis-size.h"
#include "web/axis-web-text.h"
#include "web/axis-web-storage.h"
#include "web/axis-web-auth-private.h"
#include "web/axis-web-hub.h"

G_BEGIN_DECLS

/* Tasks waiting for a thread beyond this are answered 503. */
#define AXIS_WEB_STORAGE_MAX_PENDING 16

/* The event that makes the volume list fetch itself again. */
#define AXIS_STORAGE_EVENT_CHANGED   "volumes-changed"
#define AXIS_STORAGE_EVENT_OOB       "oob"

struct _AxisWebStorage
{
	AxisStorage *storage;	/* owned reference */
	AxisWebHub  *hub;	/* nullable, not owned */
	AxisWeb     *web;	/* set when the routes are registered */
	guint        listener;
	guint        pending;	/* dispatched, not yet answered */
	gboolean     stopping;
	/* What needs attention is read again when a disk's state changed
	 * (nullable: set when the routes are registered). */
	struct _AxisAttentionPing *attention;
	/* When these pages were made (real time): a disk whose last check is
	 * before it has not been checked by this daemon. */
	gint64       started_us;
};

typedef void (*AxisWebStorageWorkFunc)(AxisWebStorage *self, gpointer data);

typedef HtmxResponse *(*AxisWebStorageDoneFunc)(
	AxisWebStorage *self,
	HtmxRequest    *request,
	gpointer        data
);

/*
 * Runs @work on a worker thread, then answers @request with what @done
 * returns, on the main loop.  Returns the deferred response for the
 * handler to return, or at once a 503 when too much is waiting or the
 * pages are stopping (then neither runs).  @free_data is called on the
 * main loop in every case.
 */
HtmxResponse *
axis_web_storage_dispatch(
	AxisWebStorage         *self,
	HtmxRequest            *request,
	AxisWebStorageWorkFunc  work,
	AxisWebStorageDoneFunc  done,
	gpointer                data,
	GDestroyNotify          free_data
);

/* ── Pieces (axis-web-storage-view.c) ───────────────────────────── */

/* Adds @child to @parent and drops the caller's reference. */
void
axis_web_storage_add(
	gpointer parent,
	gpointer child
);

/* A card with an h2 heading. */
HtmxNode *
axis_web_storage_card(const gchar *heading);

/* A paragraph with @css_class (nullable) and @text. */
HtmxNode *
axis_web_storage_text(
	const gchar *css_class,
	const gchar *text
);

/* "2026-11-12 10:15 UTC", or @never for 0. */
gchar *
axis_web_storage_format_time(
	gint64       us,
	const gchar *never
);

/* The live state of a volume: badge and explanation (id volume-N-state). */
HtmxNode *
axis_web_storage_state_block(const AxisVolumeInfo *volume);

/* Its space (id volume-N-space). */
HtmxNode *
axis_web_storage_space_block(const AxisVolumeInfo *volume);

/* When it was checked and last seen (id volume-N-checked). */
HtmxNode *
axis_web_storage_checked_block(const AxisVolumeInfo *volume);

/* The volume list, refreshed from /storage/list (id volume-list). */
HtmxNode *
axis_web_storage_volume_list(GPtrArray *volumes, gboolean admin);

/* The live fragments of one volume, for the hub. */
gchar *
axis_web_storage_render_live(const AxisVolumeInfo *volume);

/*
 * For the Workers and Conversions pages: a notice naming the volumes that
 * have no marker yet (@names, of gchar*), which no worker can recognise, and
 * when Axis writes their markers; %NULL when there are none.
 */
HtmxNode *
axis_web_storage_unmarked_notice(GPtrArray *names);

/* The same, for the volumes @storage knows now (no I/O). */
HtmxNode *
axis_web_storage_unmarked_notice_for(AxisStorage *storage);

/*
 * Why a deletion that is due (@due_us has passed) has not happened: storage
 * is read-only, or the disk @volume_id is mounted read-only.  %NULL (and for
 * a deletion not yet due) when nothing holds it back.
 */
gchar *
axis_web_storage_removal_wait_note(
	AxisStorage *storage,
	gint64       volume_id,
	gint64       due_us
);

/* A labelled text input in a div.field (not required). */
HtmxNode *
axis_web_storage_text_field(
	const gchar *id,
	const gchar *label,
	const gchar *name,
	const gchar *value,
	const gchar *placeholder,
	const gchar *help
);

/* A labelled checkbox, value "yes"; @required makes the browser insist. */
HtmxNode *
axis_web_storage_checkbox(
	const gchar *id,
	const gchar *label,
	const gchar *name,
	gboolean     required
);

/* The media type choice. */
HtmxNode *
axis_web_storage_media_select(
	const gchar *id,
	const gchar *name
);

/* A message from a fixed key in a redirect ("added", ...), or %NULL. */
const gchar *
axis_web_storage_done_message(const gchar *key);

/* The :id route parameter, or 0. */
gint64
axis_web_storage_param_id(GHashTable *params);

/* A form field as an unsigned number; FALSE when missing or malformed. */
gboolean
axis_web_storage_form_u64(
	HtmxRequest *request,
	const gchar *name,
	guint64     *value
);

/* Whether a checkbox or confirmation field says "yes". */
gboolean
axis_web_storage_form_yes(
	HtmxRequest *request,
	const gchar *name
);

/* The HTTP status for a storage error. */
guint
axis_web_storage_error_status(const GError *error);

/* An audit line naming the acting user. */
void
axis_web_storage_audit(
	HtmxRequest *request,
	const gchar *format,
	...
) G_GNUC_PRINTF(2, 3);

/* ── Pages ──────────────────────────────────────────────────────── */

void
axis_web_storage_register_volume_pages(
	AxisWebStorage *self,
	AxisWeb        *web
);

void
axis_web_storage_register_root_pages(
	AxisWebStorage *self,
	AxisWeb        *web
);

/* The volumes on the dashboard (axis-web-storage-dashboard.c). */
void
axis_web_storage_register_dashboard(
	AxisWebStorage *self,
	AxisWeb        *web
);

G_END_DECLS

#endif /* AXIS_WEB_STORAGE_PRIVATE_H */
