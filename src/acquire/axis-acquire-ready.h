/*
 * axis-acquire-ready.h - can a title be searched for and downloaded now, and
 * if not, what is missing
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * One answer, from what the layers already know, for the dashboard's
 * "Getting started" card, the notice at the top of a wanted title's page and
 * the note in the Add form:
 *
 *   metadata     a provider of films (and series) with its key
 *   indexer      an indexer source switched on, not known to be failing
 *   client       a download client switched on, not known to be failing,
 *                and which protocols the working ones take
 *   films root   a library root for films on a volume that is online and
 *                writable (asked for with AXIS_READY_FILMS)
 *   series root  the same for series (AXIS_READY_SERIES)
 *   writable     storage.read_only off: imports may write to the library
 *
 * The first four decide whether anything can be searched for and sent; the
 * last whether what arrives can be imported.  Nothing here contacts a
 * service: a source or client never checked counts as working until its
 * first contact says otherwise.
 *
 * Blocks on the database: call it from a worker thread.
 */

#ifndef AXIS_ACQUIRE_READY_H
#define AXIS_ACQUIRE_READY_H

#include <glib.h>

#include "download/axis-download.h"
#include "indexer/axis-indexer.h"
#include "meta/axis-meta.h"
#include "storage/axis-storage.h"

G_BEGIN_DECLS

typedef enum
{
	AXIS_READY_METADATA,
	AXIS_READY_INDEXER,
	AXIS_READY_CLIENT,
	AXIS_READY_FILMS_ROOT,
	AXIS_READY_SERIES_ROOT,
	AXIS_READY_WRITABLE
} AxisReadyItem;

#define AXIS_READY_N_ITEMS (AXIS_READY_WRITABLE + 1)

/* What to ask about besides the shared items. */
#define AXIS_READY_FILMS  (1u << 0)
#define AXIS_READY_SERIES (1u << 1)

/**
 * AxisReadyCheck:
 * @asked: this item was looked at (a root of a kind not asked for is not)
 * @done: it is there
 * @text: one sentence: what is there, or what is missing and why
 * @missing: (nullable): when not done, the short phrase a notice joins into
 *   its sentence ("no indexer source is set up")
 */
typedef struct
{
	gboolean  asked;
	gboolean  done;
	gchar    *text;
	gchar    *missing;
} AxisReadyCheck;

/**
 * AxisReadiness:
 * @checks: one per #AxisReadyItem
 * @usenet: a working client takes Usenet releases
 * @torrent: a working client takes torrents
 */
typedef struct
{
	AxisReadyCheck checks[AXIS_READY_N_ITEMS];
	gboolean       usenet;
	gboolean       torrent;
} AxisReadiness;

void
axis_readiness_clear(AxisReadiness *ready);

/**
 * axis_acquire_check_ready:
 * @meta, @indexers, @downloads, @storage: the layers asked
 * @kinds: %AXIS_READY_FILMS and/or %AXIS_READY_SERIES: which roots to look for
 * @ready: (out caller-allocates): the answer; clear it with
 *   axis_readiness_clear()
 *
 * Returns: %FALSE only when the database could not be read
 */
gboolean
axis_acquire_check_ready(
	AxisMeta       *meta,
	AxisIndexers   *indexers,
	AxisDownloads  *downloads,
	AxisStorage    *storage,
	guint           kinds,
	AxisReadiness  *ready,
	GError        **error
);

/* Every item asked about is there. */
gboolean
axis_readiness_complete(const AxisReadiness *ready);

/* Whether a title of @kinds (one of them) can be searched for and sent now:
 * an indexer source, a download client and its root (the metadata it was
 * added with is kept, so its key is not needed for this). */
gboolean
axis_readiness_can_get(
	const AxisReadiness *ready,
	guint                kinds
);

/* What the checklist calls @item: "A metadata key", "An indexer". */
const gchar *
axis_ready_item_label(AxisReadyItem item);

/* Where an administrator sets @item up: a local path. */
const gchar *
axis_ready_item_href(AxisReadyItem item);

/* The words of that link: "Add a key in Settings". */
const gchar *
axis_ready_item_action(AxisReadyItem item);

G_END_DECLS

#endif /* AXIS_ACQUIRE_READY_H */
