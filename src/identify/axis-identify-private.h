/*
 * axis-identify-private.h - shared by the files of the identification layer
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 *   axis-identify.c        lifetime, settings, owners, the scheduler
 *   axis-identify-ids.c    identifiers in names and .nfo bytes (pure)
 *   axis-identify-rules.c  names, units, the decision, quality (pure)
 *   axis-identify-run.c    one root's run: scopes, units, evidence, storing
 *   axis-identify-store.c  the pages' reads, a person's actions, bulk adding
 */

#ifndef AXIS_IDENTIFY_PRIVATE_H
#define AXIS_IDENTIFY_PRIVATE_H

#include "identify/axis-identify.h"

#include "db/axis-db-result.h"

G_BEGIN_DECLS

struct _AxisIdentify
{
	gint                         refs;	/* atomic */
	AxisDbPool                  *pool;
	AxisSettings                *settings;	/* (nullable) owned */
	AxisStorage                 *storage;	/* owned */
	AxisMeta                    *meta;	/* owned */
	/* By kind (AXIS_META_KIND_FILM, _SERIES): (nullable) each. */
	const AxisIdentifyOwnerInfo *owners[3];
	gpointer                     owner_data[3];
	GMutex                       lock;
	AxisIdentifyTuning           tuning;	/* (lock) */
};

/* The owner of @kind, or %NULL; *@data its data. */
const AxisIdentifyOwnerInfo *
axis_identify_owner(
	AxisIdentify *self,
	AxisMetaKind  kind,
	gpointer     *data
);

/* An integer setting, or @fallback without a store. */
gint64
axis_identify_setting_integer(
	AxisIdentify *self,
	const gchar  *key,
	gint64        fallback
);

void
axis_identify_get_tuning(
	AxisIdentify       *self,
	AxisIdentifyTuning *tuning
);

/* "%" G_GINT64_FORMAT */
gchar *
axis_identify_int_text(gint64 value);

/* Valid UTF-8, control characters made spaces, at most @max characters;
 * %NULL for %NULL or nothing left. */
gchar *
axis_identify_text_cut(
	const gchar *text,
	guint        max
);

gint64
axis_identify_row_int(
	AxisDbResult *result,
	guint         row,
	guint         column,
	gint64        fallback
);

gchar *
axis_identify_row_text(
	AxisDbResult *result,
	guint         row,
	guint         column
);

/* The first provider that serves films, or %NULL. */
const AxisMetaProviderInfo *
axis_identify_film_provider(AxisIdentify *self);

/* The first provider that serves @kind, or %NULL. */
const AxisMetaProviderInfo *
axis_identify_provider_for(
	AxisIdentify *self,
	AxisMetaKind  kind
);

/* "movie" or "series"; "movies" or "series". */
const gchar *
axis_identify_noun(AxisMetaKind kind);

const gchar *
axis_identify_nouns(AxisMetaKind kind);

/* Reasons as stored (a JSON array of {"c", "t"}) and back. */
gchar *
axis_identify_reasons_to_json(const GPtrArray *reasons);

GPtrArray *
axis_identify_reasons_from_json(const gchar *json);

/* The owner's links of a unit of a root of @kind, in the caller's
 * transaction. */
gboolean
axis_identify_owner_link(
	AxisIdentify  *self,
	AxisMetaKind   kind,
	AxisDbConn    *conn,
	gint64         identity_id,
	gint64         title_id,
	const gchar   *user,
	GError       **error
);

gboolean
axis_identify_owner_unlink(
	AxisIdentify  *self,
	AxisMetaKind   kind,
	AxisDbConn    *conn,
	gint64         identity_id,
	const gchar   *user,
	GError       **error
);

/* The SQL of a root's videos that count (not samples, not extras, by the
 * analysis module's own rules), over library_file f and library_root r. */
const gchar *
axis_identify_counted_condition(void);

/*
 * The SQL, over library_identity i, of a unit to look at again as if its
 * folder had changed, no person's decision held:
 *  - one whose wait for the provider is over (retry_at passed): a full
 *    record its decision needs was not answered (a unit not linked waits in
 *    state 'waiting'; a linked one keeps its link meanwhile);
 *  - one decided by older rules than AXIS_IDENTIFY_RULES_VERSION
 *    (library_identity.rules, 0052_identify_hunt_fixes.sql), once: held for
 *    review (unknown or likely), or, decided before
 *    AXIS_IDENTIFY_RULES_SERIES_FIT, a series' folder linked by itself by its
 *    episodes (rule 5, whose comparison of the series of a name was wrong
 *    before version 2; version 3 did not change it, so those links are not
 *    looked at again for it).  (Version 1 covers the series' folders held by
 *    the year rule of films, which fix-series-match-without-year looked at
 *    again by the missing reason "files".)
 * A person's decision is never looked at again, and a look again never
 * removes or changes a link.
 */
#define AXIS_IDENTIFY_AGAIN \
	"((NOT i.held AND i.retry_at IS NOT NULL AND i.retry_at <= now())" \
	" OR (NOT i.held AND i.rules < " G_STRINGIFY(AXIS_IDENTIFY_RULES_VERSION) " AND i.state = 'review')" \
	" OR (NOT i.held AND i.rules < " G_STRINGIFY(AXIS_IDENTIFY_RULES_SERIES_FIT) " AND i.state = 'linked'" \
	" AND i.linked_how = 'auto' AND i.reasons @> '[{\"c\": \"series_fits\"}]'::jsonb))"

/* Queues a run of @root_id on @conn (de-duplicated by the root). */
gboolean
axis_identify_enqueue(
	AxisDbConn  *conn,
	gint64       root_id,
	gint         priority,
	gboolean    *created,
	GError     **error
);

G_END_DECLS

#endif /* AXIS_IDENTIFY_PRIVATE_H */
