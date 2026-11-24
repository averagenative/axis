/*
 * axis-acquire-private.h - shared by the files of the acquisition layer
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 *   axis-acquire.c          lifetime, settings, small helpers, the name key,
 *                           reasons, the blocklist, the stored searches,
 *                           pacing
 *   axis-acquire-search.c   the search-and-grab job, the manual grab
 *   axis-acquire-hand.c     a manual search's groups, and what a grab by
 *                           hand would do
 *   axis-acquire-follow.c   the follow-up of grabs
 *   axis-acquire-replace.c  what stands in the way of a grab by hand, and
 *                           the replace of the download in flight
 */

#ifndef AXIS_ACQUIRE_PRIVATE_H
#define AXIS_ACQUIRE_PRIVATE_H

#include "acquire/axis-acquire.h"

G_BEGIN_DECLS

#define AXIS_ACQUIRE_US(column) "(extract(epoch FROM " column ") * 1000000)::bigint"

/* The downloads layer's states of a download still at its client. */
#define AXIS_ACQUIRE_LIVE_STATES "('sending', 'queued', 'downloading', 'paused')"

struct _AxisAcquire
{
	AxisDbPool                 *pool;
	AxisSettings               *settings;	/* (nullable) owned */
	AxisIndexers               *indexers;	/* owned */
	AxisDownloads              *downloads;	/* owned */
	AxisStorage                *storage;	/* owned */
	AxisCompat                 *compat;	/* (nullable) borrowed */
	const AxisAcquireOwnerInfo *info;
	gpointer                    data;

	GMutex                      hook_lock;
	AxisAcquireClaimHook        claim_hook;	/* (hook_lock) */
	gpointer                    claim_hook_data;
	AxisAcquireClaimHook        replace_hook;	/* (hook_lock) */
	gpointer                    replace_hook_data;
	/* No prune of old searches before this (monotonic; 0: none yet). */
	gint64                      prune_after_us;	/* (hook_lock) */
};

/* An SQL statement with the module's table and column names put in:
 * "%T" the owner table, "%O" the owner column, "%S" the search table, "%C"
 * the candidate table, "%G" the grab table, "%B" the blocklist table, "%P"
 * the pace table, "%J" the search job's key expression (alias s), "%K" the
 * grab's (alias g), "%M" the module, "%%" a per cent sign. */
gchar *
axis_acquire_sql(
	AxisAcquire *self,
	const gchar *pattern
);

/* The history, through the module (a printf-style text). */
gboolean
axis_acquire_eventf(
	AxisAcquire            *self,
	AxisDbConn             *conn,
	gint64                  owner_id,
	const AxisAcquireWant  *want,
	const gchar            *kind,
	const gchar            *user,
	gint64                  search_id,
	gint64                  grab_id,
	GError                **error,
	const gchar            *format,
	...
) G_GNUC_PRINTF(10, 11);

/* What a candidate row is read from (alias c), and reading it. */
#define AXIS_ACQUIRE_CANDIDATE_COLUMNS \
	"c.id, c.search_id, c.position, c.release_id, c.source_id, c.name, c.protocol, c.size_bytes, " \
	AXIS_ACQUIRE_US("c.published_at") ", c.seeders, c.indexer, c.indexer_id, c.info_hash, c.name_key, c.quality," \
	" c.match, c.match_text, c.accepted, c.safety, c.blocklisted, c.rank, c.score, c.reasons::text, c.note"

AxisAcquireCandidate *
axis_acquire_candidate_from_row(
	AxisDbResult *result,
	guint         row
);

/* Which protocols an enabled client takes (one that refused its secret is
 * not counted). */
gboolean
axis_acquire_read_clients(
	AxisAcquire  *self,
	gboolean     *usenet,
	gboolean     *torrent,
	GError      **error
);

/* The module's hand_preview() into @preview, then whether the release is
 * better than each file it replaces and so whether a tick is needed. */
gboolean
axis_acquire_preview_fill(
	AxisAcquire                *self,
	AxisDbConn                 *conn,
	AxisAcquireWant            *want,
	const AxisAcquireCandidate *candidate,
	AxisAcquireHandPreview     *preview,
	GError                    **error
);

/**
 * AxisAcquireHow:
 * @by_hand: Grab this one: the target is by hand, the module's refusals
 *   (hand_preview()'s) and the ticks are checked
 * @ticks: (nullable): the ticks, with @by_hand
 * @grabbed: (nullable) (out): what was recorded, with @by_hand
 * @why: (nullable): the grab's reason; else Grab it anyway's
 * @force: (nullable): Grab it by force (with @by_hand): the fixed rules
 *   about the name are set aside, the digest and its ticks are checked
 *
 * How a person grabs a stored release.
 */
typedef struct
{
	gboolean                      by_hand;
	const AxisAcquireHandTicks   *ticks;
	AxisAcquireHandGrabbed       *grabbed;
	const gchar                  *why;
	const AxisAcquireForceTicks  *force;
} AxisAcquireHow;

/* The files of @preview the release is not better than (@all: every file it
 * would replace), as stored with the grab (replaces_files); *@n how many. */
gchar *
axis_acquire_replaces_json(
	const AxisAcquireHandPreview *preview,
	gboolean                      all,
	guint                        *n
);

/* The fixed rules of @candidate as a grab by force records them
 * ([{"c": code, "t": text}]); *@n how many.  With @one_file, the person's
 * choice that one file holds the parts they named follows them, as
 * {"k": "choice", "c": "one_file"} (not counted in *@n). */
gchar *
axis_acquire_forced_json(
	const AxisAcquireCandidate *candidate,
	gboolean                    one_file,
	guint                      *n
);

/* The words of the fixed rules that rule @candidate out. */
gchar *
axis_acquire_safety_words(const AxisAcquireCandidate *candidate);

/* The digest of what a force confirmation shows: the release @candidate, the
 * rules set aside, the owner it is bound to, what it fills and every file
 * it would replace with its quality (@preview's), and whether a grab is in
 * flight.  Of @want: the owner, the title it is of (its id and label) and its
 * quality profile, so that "I have checked that this release is X" cannot be
 * applied after the movie was identified as another title or given another
 * profile. */
gchar *
axis_acquire_force_digest(
	const AxisAcquireCandidate   *candidate,
	const AxisAcquireHandPreview *preview,
	const AxisAcquireWant        *want
);

/* axis_acquire_handle_failure(), telling its caller how the removal went.
 * *@gone: (optional) nothing of the download is at its client now.  *@why:
 * (optional) (transfer full) (nullable) when the client was asked and could
 * not remove it, or was not asked (AxisAcquireFailure.judge_place), the
 * words recorded on the grab. */
gboolean
axis_acquire_handle_failure_full(
	AxisAcquire               *self,
	gint64                     owner_id,
	gint64                     grab_id,
	const AxisAcquireFailure  *failure,
	const gchar               *user,
	gboolean                   completed_too,
	gboolean                  *handled,
	gboolean                  *gone,
	gchar                    **why,
	AxisAcquireFollowReport   *report,
	GError                   **error
);

/*
 * What stands in the way of a grab by hand of @candidate, into @preview
 * (axis-acquire-replace.c): nothing when @preview is refused for another
 * reason (that obstacle is what the page says, and no replace is offered).
 * Else the grabs in the way (the module's standing(), or the owner's open
 * grabs and those still being removed), each read as it is now; in_flight,
 * the sentences, the links, and whether the one grab can be replaced, with
 * the offer's digest.  Not replaceable: @preview->refused is the sentences.
 * Read only.
 */
gboolean
axis_acquire_standing_fill(
	AxisAcquire                *self,
	AxisDbConn                 *conn,
	AxisAcquireWant            *want,
	const AxisAcquireCandidate *candidate,
	AxisAcquireHandPreview     *preview,
	GError                    **error
);

/*
 * Ends the grab in flight @old for a replace by @user with @new_release:
 * axis_acquire_handle_failure() with the close reason "replaced", no block,
 * no search, the grab taken only while its download has not completed and
 * has no import, and the place judged before the client is asked.
 * *@ended: the grab was closed (%FALSE: it had arrived, or was dealt with
 * already: nothing changed).  *@gone: nothing of its download is at its
 * client now.  *@why: (nullable) why not, when the client did not remove it.
 */
gboolean
axis_acquire_replace_end(
	AxisAcquire                *self,
	gint64                      owner_id,
	const AxisAcquireStanding  *old,
	const gchar                *new_release,
	const gchar                *user,
	gboolean                   *ended,
	gboolean                   *gone,
	gchar                     **why,
	GError                    **error
);

gboolean
axis_acquire_grab_stored(
	AxisAcquire              *self,
	const AxisAcquireTarget  *target,
	gint64                    candidate_id,
	const gchar              *user,
	const AxisAcquireHow     *how,
	gint64                   *grab_id,
	AxisAcquireGrabResult    *result,
	gchar                   **problem,
	GError                  **error
);

G_END_DECLS

#endif /* AXIS_ACQUIRE_PRIVATE_H */
