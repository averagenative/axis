/*
 * axis-quality-store.h - quality profiles in the database: the built-ins
 * written from code, copies, saves, deletion when nothing uses one, and the
 * registry of what uses them
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The table is migration 0022's.  What this keeps true besides what the
 * database itself refuses:
 *
 *  - Every stored definition is read through the one strict reader
 *    (axis_quality_profile_parse()).  A row that fails, or that a newer
 *    version wrote in a later format, is *unusable*: it gives no profile (so
 *    nothing is decided with it), cannot be copied or edited, and @unusable
 *    says why for its page.  Nothing of it is used, and nothing repairs it.
 *  - The built-ins are written at start-up from the code table
 *    (axis_quality_store_sync_builtins()); nothing else writes one, and one
 *    is never edited or deleted (copy it).
 *  - A profile is deleted only when nothing uses it.  What uses profiles is
 *    a registry: a module that refers to profiles (the movies module's
 *    films, later the series) adds a statement that lists the profiles it
 *    uses (axis_quality_store_add_user()), and the default-profile setting
 *    counts too.  The check runs in the transaction that deletes, with the
 *    profile's row locked; the modules' own tables refer to quality_profile
 *    with ON DELETE RESTRICT besides.
 *  - A save is refused when another was made since the form was opened.
 *
 * Every function that takes a store or a connection blocks: call it from a
 * worker thread.  Create the store, and add users, on the main thread
 * before it is used.
 */

#ifndef AXIS_QUALITY_STORE_H
#define AXIS_QUALITY_STORE_H

#include <glib.h>

#include "core/axis-setting.h"
#include "core/axis-settings.h"
#include "db/axis-db-pool.h"
#include "quality/axis-quality.h"

G_BEGIN_DECLS

#define AXIS_QUALITY_MODULE_ID       "quality"
#define AXIS_QUALITY_SETTING_DEFAULT "quality.default_profile"

#define AXIS_QUALITY_ERROR (axis_quality_error_quark())

/**
 * AxisQualityError:
 * @AXIS_QUALITY_ERROR_NOT_FOUND: no such profile
 * @AXIS_QUALITY_ERROR_BUILT_IN: a built-in is never edited or deleted
 * @AXIS_QUALITY_ERROR_INVALID: the definition or the name is refused
 * @AXIS_QUALITY_ERROR_UNUSABLE: the stored definition does not validate
 * @AXIS_QUALITY_ERROR_CONFLICT: another save was made in between
 * @AXIS_QUALITY_ERROR_IN_USE: something uses the profile
 */
typedef enum
{
	AXIS_QUALITY_ERROR_NOT_FOUND,
	AXIS_QUALITY_ERROR_BUILT_IN,
	AXIS_QUALITY_ERROR_INVALID,
	AXIS_QUALITY_ERROR_UNUSABLE,
	AXIS_QUALITY_ERROR_CONFLICT,
	AXIS_QUALITY_ERROR_IN_USE
} AxisQualityError;

GQuark
axis_quality_error_quark(void);

typedef struct _AxisQualityStore AxisQualityStore;

/* @pool must outlive the store; @settings (nullable, referenced) holds the
 * default-profile setting once a module has declared it. */
AxisQualityStore *
axis_quality_store_new(
	AxisDbPool   *pool,
	AxisSettings *settings
);

AxisQualityStore *
axis_quality_store_ref(AxisQualityStore *self);

void
axis_quality_store_unref(AxisQualityStore *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisQualityStore, axis_quality_store_unref)

AxisDbPool *
axis_quality_store_get_pool(AxisQualityStore *self);

/* The module's settings (the default profile for a new title), ended by an
 * entry whose key is %NULL; its check is registered too. */
const AxisSettingInfo *
axis_quality_store_settings(void);

/**
 * axis_quality_store_add_user:
 * @one: what one user is called, for messages ("film")
 * @many: what several are called ("films")
 * @sql: one SELECT statement (no ';') whose only column is the id of a
 *   quality_profile that is in use, a row per use; a constant of the
 *   module's own code, never built from input
 *
 * Something that uses profiles, so that one in use is not deleted.  On the
 * main thread, before the store is used.  Returns: %FALSE (and a critical)
 * for a statement that is not of that form.
 */
gboolean
axis_quality_store_add_user(
	AxisQualityStore *self,
	const gchar      *one,
	const gchar      *many,
	const gchar      *sql
);

/* The slug of the default profile for a new title: the setting, or
 * %AXIS_QUALITY_DEFAULT_SLUG while none is declared.  Any thread. */
gchar *
axis_quality_store_default_slug(AxisQualityStore *self);

/**
 * AxisQualityStored:
 * @copied_from: (nullable): the slug of the profile it was copied from
 * @usable: the definition validated; @profile is it
 * @unusable: (nullable): why not, a sentence, when not usable
 */
typedef struct
{
	gint64              id;
	gchar              *slug;
	gchar              *name;
	gboolean            built_in;
	gint                format;
	gchar              *text;
	gint                revision;
	gchar              *copied_from;
	gint64              created_us;
	gint64              updated_us;
	gchar              *updated_by;
	gboolean            usable;
	gchar              *unusable;
	AxisQualityProfile  profile;
} AxisQualityStored;

void
axis_quality_stored_free(AxisQualityStored *stored);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisQualityStored, axis_quality_stored_free)

/* A row's definition checked: whether it is usable and, if not, why (the
 * sentence the pages show).  Pure: for the store and its tests. */
gboolean
axis_quality_stored_check(
	gint                format,
	const gchar        *text,
	AxisQualityProfile *profile,
	gchar             **unusable
);

/* ── Reading (on a connection the caller holds) ─────────────────── */

/* By slug or by id; %NULL (and %TRUE) for none. */
gboolean
axis_quality_store_read(
	AxisDbConn         *conn,
	const gchar        *slug,
	AxisQualityStored **stored,
	GError            **error
);

gboolean
axis_quality_store_read_id(
	AxisDbConn         *conn,
	gint64              id,
	AxisQualityStored **stored,
	GError            **error
);

/**
 * axis_quality_store_load:
 *
 * For a module about to decide: the profile @id, usable, or an error
 * (%AXIS_QUALITY_ERROR_NOT_FOUND, %AXIS_QUALITY_ERROR_UNUSABLE with why).
 */
gboolean
axis_quality_store_load(
	AxisDbConn         *conn,
	gint64              id,
	AxisQualityProfile *profile,
	GError            **error
);

/* ── Reading (through the store's pool) ─────────────────────────── */

/* Every profile: the built-ins first in their order, then the others by
 * age (AxisQualityStored). */
gboolean
axis_quality_store_list(
	AxisQualityStore  *self,
	GPtrArray        **profiles,
	GError           **error
);

gboolean
axis_quality_store_get(
	AxisQualityStore   *self,
	const gchar        *slug,
	AxisQualityStored **stored,
	GError            **error
);

/**
 * axis_quality_store_uses:
 * @uses: (out) (transfer full) (element-type utf8): what uses it, in words
 *   ("3 films", "the default for new titles"); empty when nothing does
 */
gboolean
axis_quality_store_uses(
	AxisQualityStore  *self,
	const gchar       *slug,
	GPtrArray        **uses,
	GError           **error
);

/* ── Writing ────────────────────────────────────────────────────── */

/* Writes the built-ins from the code table (in one transaction): a missing
 * one is added, one whose definition or name differs from the code is
 * brought up to date.  @changed counts the rows written. */
gboolean
axis_quality_store_sync_builtins(
	AxisDbPool  *pool,
	guint       *changed,
	GError     **error
);

/* A new profile with the definition of @source, named @name (already
 * cleaned).  Its slug in @slug. */
gboolean
axis_quality_store_copy(
	AxisQualityStore  *self,
	const gchar       *source,
	const gchar       *name,
	const gchar       *user,
	gchar            **slug,
	GError           **error
);

/**
 * axis_quality_store_save:
 * @expected: the revision the edit was made from (another fails with
 *   %AXIS_QUALITY_ERROR_CONFLICT)
 * @name: already cleaned
 * @profile: the new definition, checked here again
 * @changed: (out) (optional): whether anything was written (an unchanged
 *   name and definition write nothing)
 * @problems: (nullable): why the definition was refused
 */
gboolean
axis_quality_store_save(
	AxisQualityStore         *self,
	const gchar              *slug,
	gint                      expected,
	const gchar              *name,
	const AxisQualityProfile *profile,
	const gchar              *user,
	gboolean                 *changed,
	GPtrArray                *problems,
	GError                  **error
);

/**
 * axis_quality_store_delete:
 * @uses: (out) (optional) (transfer full) (element-type utf8): with
 *   %AXIS_QUALITY_ERROR_IN_USE, what uses it
 *
 * Refuses a built-in (%AXIS_QUALITY_ERROR_BUILT_IN) and a profile in use
 * (%AXIS_QUALITY_ERROR_IN_USE).
 */
gboolean
axis_quality_store_delete(
	AxisQualityStore  *self,
	const gchar       *slug,
	GPtrArray        **uses,
	GError           **error
);

G_END_DECLS

#endif /* AXIS_QUALITY_STORE_H */
