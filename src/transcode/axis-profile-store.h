/*
 * axis-profile-store.h - conversion profiles in the database: the
 * built-ins written from code, revisions, the lock, assignments
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The tables are migration 0014's.  What this keeps true besides what the
 * database itself refuses:
 *
 *  - Every stored definition is read through the one strict function
 *    (axis_profile_definition_parse()) and must also be in the canonical
 *    form this version writes, hash to its stored hash and be of its
 *    profile's kind.  A revision that fails is *unusable*: it gives no
 *    target (so no estimate, no eligibility), cannot be copied, assigned or
 *    edited from, and @unusable says why for its page.  Nothing repairs it.
 *  - The built-ins are written at start-up from the code table
 *    (axis_profile_store_sync_builtins()): a definition or name that changed
 *    in the code is a new revision, the old one kept.  Nothing else writes
 *    a built-in's revision.
 *  - An edit is a new revision, or nothing when nothing changed.  It keeps
 *    the earlier revision's proof only when axis_profile_carries_proof()
 *    says so.  Nothing here proves a revision: an administrator can, by
 *    hand from passed trials (transcode/axis-conversion.h, a proof of
 *    method "trials"); the canary change will add proof on devices.
 *  - Assigning records intent: it changes no file, creates no job and opens
 *    nothing.
 *
 * Every function blocks: call it from a worker thread.
 */

#ifndef AXIS_PROFILE_STORE_H
#define AXIS_PROFILE_STORE_H

#include <glib.h>

#include "db/axis-db-pool.h"
#include "db/axis-db-result.h"
#include "transcode/axis-profile.h"

G_BEGIN_DECLS

#define AXIS_PROFILE_ERROR (axis_profile_error_quark())

/**
 * AxisProfileError:
 * @AXIS_PROFILE_ERROR_NOT_FOUND: no such profile, revision or root
 * @AXIS_PROFILE_ERROR_BUILT_IN: a built-in is never edited or archived
 * @AXIS_PROFILE_ERROR_INVALID: the definition, name or note is refused
 * @AXIS_PROFILE_ERROR_UNUSABLE: the revision's stored definition does not
 *   validate
 * @AXIS_PROFILE_ERROR_CONFLICT: another revision was saved in between
 * @AXIS_PROFILE_ERROR_ARCHIVED: the profile is archived
 * @AXIS_PROFILE_ERROR_ASSIGNED: an assigned profile is not archived
 * @AXIS_PROFILE_ERROR_MISMATCH: the root holds what the profile is not for
 * @AXIS_PROFILE_ERROR_CONFIRM: the verdict needs a second confirmation
 */
typedef enum
{
	AXIS_PROFILE_ERROR_NOT_FOUND,
	AXIS_PROFILE_ERROR_BUILT_IN,
	AXIS_PROFILE_ERROR_INVALID,
	AXIS_PROFILE_ERROR_UNUSABLE,
	AXIS_PROFILE_ERROR_CONFLICT,
	AXIS_PROFILE_ERROR_ARCHIVED,
	AXIS_PROFILE_ERROR_ASSIGNED,
	AXIS_PROFILE_ERROR_MISMATCH,
	AXIS_PROFILE_ERROR_CONFIRM
} AxisProfileError;

GQuark
axis_profile_error_quark(void);

/**
 * AxisProfileRevision:
 *
 * One revision as read, with its profile's identity.  Reference counted,
 * read-only once made.
 */
typedef struct
{
	gint                  refs;	/* atomic */
	gint64                id;
	gint64                profile_id;
	gint                  number;
	gchar                *slug;
	AxisProfileKind       kind;
	gboolean              built_in;
	gboolean              archived;
	gboolean              current;
	gchar                *name;
	gchar                *note;
	gchar                *text;	/* the definition as stored */
	gchar                *hash;
	gchar                *origin;	/* "code", "user" */
	gchar                *created_by;	/* nullable */
	gint64                created_us;
	gboolean              usable;
	gchar                *unusable;	/* why not, a sentence; NULL when usable */
	AxisProfileDefinition def;	/* only when usable */
	gboolean              unlocked;	/* a proof exists */
	gchar                *proof_method;	/* "canary", "carried", "trials", or NULL */
	gint64                proven_us;
} AxisProfileRevision;

AxisProfileRevision *
axis_profile_revision_ref(AxisProfileRevision *revision);

void
axis_profile_revision_unref(AxisProfileRevision *revision);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisProfileRevision, axis_profile_revision_unref)

/* Whether a revision may replace files: usable, and proven (by hand from
 * passed trials, on devices, or carried from a revision that was). */
gboolean
axis_profile_revision_is_unlocked(const AxisProfileRevision *revision);

/* The revision's target (axis_profile_to_target(), with its name and
 * revision); FALSE, and @target zero, for an unusable revision. */
gboolean
axis_profile_revision_target(
	const AxisProfileRevision *revision,
	AxisEstimateTarget        *target
);

/* The columns (and joins) a revision is read with: a query that selects
 * AXIS_PROFILE_REVISION_COLUMNS from AXIS_PROFILE_REVISION_FROM (aliases r,
 * p, pr) hands each row to axis_profile_revision_from_row(). */
#define AXIS_PROFILE_REVISION_COLUMNS \
	"r.id, r.profile_id, r.number, p.slug, p.kind, p.built_in, p.archived, p.current_revision_id = r.id," \
	" r.name, r.note, r.definition, r.hash, r.origin, r.created_by," \
	" (extract(epoch FROM r.created_at) * 1000000)::bigint, pr.method," \
	" coalesce((extract(epoch FROM pr.proven_at) * 1000000)::bigint, 0)"
#define AXIS_PROFILE_REVISION_N_COLUMNS 17
#define AXIS_PROFILE_REVISION_FROM \
	" FROM conversion_profile_revision r JOIN conversion_profile p ON p.id = r.profile_id" \
	" LEFT JOIN conversion_profile_proof pr ON pr.revision_id = r.id"

/* A revision from the row @row of @result, starting at @column; checked
 * (usable or not, with why). */
AxisProfileRevision *
axis_profile_revision_from_row(
	AxisDbResult *result,
	guint         row,
	guint         column
);

/* ── Reading ────────────────────────────────────────────────────── */

/* The current revision of the profile @slug; NULL (and TRUE) for none. */
gboolean
axis_profile_store_current(
	AxisDbConn           *conn,
	const gchar          *slug,
	AxisProfileRevision **revision,
	GError              **error
);

/* Revision @number of @slug (0: the current one); NULL (and TRUE) for none. */
gboolean
axis_profile_store_revision(
	AxisDbConn           *conn,
	const gchar          *slug,
	gint                  number,
	AxisProfileRevision **revision,
	GError              **error
);

/* By id; NULL (and TRUE) for none. */
gboolean
axis_profile_store_revision_by_id(
	AxisDbConn           *conn,
	gint64                id,
	AxisProfileRevision **revision,
	GError              **error
);

/* Every revision of @slug, newest first (AxisProfileRevision). */
gboolean
axis_profile_store_history(
	AxisDbConn   *conn,
	const gchar  *slug,
	GPtrArray   **revisions,
	GError      **error
);

/**
 * AxisProfileAssignment:
 * @revision_id: what was current when assigned
 */
typedef struct
{
	gint64           root_id;
	gchar           *root_name;
	gchar           *media_type;
	AxisProfileKind  kind;
	gint64           profile_id;
	gchar           *slug;
	gint64           revision_id;
	gint64           current_revision_id;
	gchar           *verdict;
	gboolean         confirmed;
	gchar           *assigned_by;
	gint64           assigned_us;
} AxisProfileAssignment;

void
axis_profile_assignment_free(AxisProfileAssignment *assignment);

/* Every assignment, by root name (AxisProfileAssignment). */
gboolean
axis_profile_store_assignments(
	AxisDbConn   *conn,
	GPtrArray   **assignments,
	GError      **error
);

/**
 * AxisProfileInfo:
 * @current: (nullable): the current revision (NULL only for a profile
 *   whose row has none, which the schema does not allow to stay)
 */
typedef struct
{
	gint64               id;
	gchar               *slug;
	AxisProfileKind      kind;
	gboolean             built_in;
	gboolean             archived;
	gchar               *copied_from;	/* the slug, or NULL */
	guint                revisions;
	AxisProfileRevision *current;
} AxisProfileInfo;

void
axis_profile_info_free(AxisProfileInfo *info);

/* Every profile, built-ins first (in their order), then the others by age;
 * archived ones only when asked for (AxisProfileInfo). */
gboolean
axis_profile_store_list(
	AxisDbConn   *conn,
	gboolean      with_archived,
	GPtrArray   **profiles,
	GError      **error
);

/* ── Writing ────────────────────────────────────────────────────── */

/* Writes the built-ins from the code table (in one transaction): a missing
 * one is added, one whose current revision differs from the code (in
 * definition or name) gets a new revision.  @added counts revisions
 * written. */
gboolean
axis_profile_store_sync_builtins(
	AxisDbPool  *pool,
	guint       *added,
	GError     **error
);

/* A new profile with the definition of @source's current revision, named
 * @name (already cleaned), locked.  Its slug in @slug. */
gboolean
axis_profile_store_copy(
	AxisDbPool   *pool,
	const gchar  *source,
	const gchar  *name,
	const gchar  *user,
	gchar       **slug,
	GError      **error
);

/**
 * AxisProfileSaved:
 * @created: a revision was added (FALSE: nothing changed)
 * @number: the profile's current revision number after
 * @revision_id: its id
 * @carried: the earlier revision's proof was carried forward
 */
typedef struct
{
	gboolean created;
	gint     number;
	gint64   revision_id;
	gboolean carried;
} AxisProfileSaved;

/**
 * axis_profile_store_save:
 * @expected: the revision number the edit was made from (a different
 *   current one fails with %AXIS_PROFILE_ERROR_CONFLICT)
 * @def: the new definition, checked here again
 * @name: (already cleaned) and @note (cleaned, may be "")
 * @problems: (nullable): why the definition was refused
 */
gboolean
axis_profile_store_save(
	AxisDbPool                  *pool,
	const gchar                 *slug,
	gint                         expected,
	const AxisProfileDefinition *def,
	const gchar                 *name,
	const gchar                 *note,
	const gchar                 *user,
	AxisProfileSaved            *saved,
	GPtrArray                   *problems,
	GError                     **error
);

gboolean
axis_profile_store_set_archived(
	AxisDbPool   *pool,
	const gchar  *slug,
	gboolean      archived,
	GError      **error
);

/**
 * axis_profile_store_assign:
 * @verdict: "safe", "risky" or "unknown": the verdict for the root and the
 *   profile's current revision, worked out by the caller now
 * @confirmed: the administrator confirmed it (needed unless safe)
 *
 * Fails with %AXIS_PROFILE_ERROR_CONFIRM for a verdict other than safe
 * without @confirmed, %AXIS_PROFILE_ERROR_MISMATCH for a root the profile is
 * not for, %AXIS_PROFILE_ERROR_ARCHIVED, %AXIS_PROFILE_ERROR_UNUSABLE.
 */
gboolean
axis_profile_store_assign(
	AxisDbPool   *pool,
	gint64        root_id,
	const gchar  *slug,
	const gchar  *verdict,
	gboolean      confirmed,
	const gchar  *user,
	GError      **error
);

gboolean
axis_profile_store_unassign(
	AxisDbPool       *pool,
	gint64            root_id,
	AxisProfileKind   kind,
	gboolean         *removed,
	GError          **error
);

G_END_DECLS

#endif /* AXIS_PROFILE_STORE_H */
