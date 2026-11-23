/*
 * axis-web-attention.h - what needs a person: the bell in the header and the
 * page "Needs attention"
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * (add-attention-notices, slice N1.)  A notice is worked out from the state
 * Axis already keeps (a disk's state, an import's state, a client's state)
 * and is never stored: it is gone when the state behind it has ended.  A
 * module registers a source from its register_routes
 * (axis_web_add_attention_source()), as it registers a dashboard part; the
 * shell runs every source of a switched-on module on one worker thread, one
 * after another, and keeps what they gave in memory (the board).  The header
 * of every page and the page /attention are drawn from the board alone: no
 * page view runs a source or a database statement for them.
 *
 * The board is read again when something that can change it happened (a job
 * ended, a disk's state changed, a person acted), no sooner than
 * %AXIS_ATTENTION_MIN_GAP_S after the last reading, and every
 * %AXIS_ATTENTION_PERIOD_S whatever happened, because the waits below are
 * about time passing.  When a reading changed the board, the event
 * %AXIS_ATTENTION_EVENT is published on the topic %AXIS_ATTENTION_TOPIC with
 * no notice in it; each open page then asks for its own bell
 * (/attention/bell), since what a user sees depends on who they are and on
 * what they dismissed.
 *
 * Who sees what.  An item says whether it is an administrator's
 * (@admin_only, set unless a source clears it).  Every item of this slice
 * is: each is ended by an action only an administrator has.  So a user who is
 * not an administrator has no bell, and /attention, /attention/bell and the
 * two posts refuse them.  An item that is not an administrator's (a later
 * slice may add one) must link to a page any user can open and say nothing
 * that page would not show them.
 *
 * Slice N2.  The dashboard's card "Needs attention" is the head of the same
 * list (the first %AXIS_ATTENTION_CARD_ROWS notices of the user who looks),
 * drawn from the board like the bell: no dashboard part adds a line to it,
 * and a module says what needs a person through a source alone.  A source
 * runs one statement, so a module with several things to say registers
 * several sources; one that fails keeps its own notices and no other's.
 *
 * "Attention" is this feature's name in code; "notice" is taken by the line
 * a page draws after an action (web/axis-web-notice.h).
 */

#ifndef AXIS_WEB_ATTENTION_H
#define AXIS_WEB_ATTENTION_H

#include <glib.h>

#include "db/axis-db-pool.h"
#include "web/axis-web.h"

G_BEGIN_DECLS

/* Only administrators may follow it (the hub refuses a topic that ends
 * ":admin" to anyone else): every notice is an administrator's, and even
 * "something changed" is theirs to know. */
#define AXIS_ATTENTION_TOPIC      "attention:admin"
#define AXIS_ATTENTION_EVENT      "attention"

/* A stale board is read again no sooner than this after the last reading,
 * and every period whatever happened. */
#define AXIS_ATTENTION_MIN_GAP_S  10
#define AXIS_ATTENTION_PERIOD_S   60

/*
 * The waits: how long Axis has been trying before a state it keeps retrying
 * becomes a notice.  A disk is checked every minute, a download client polled
 * every minute, an indexer contacted only when it is used (so it also needs a
 * second contact that found it in the same state).  A state only a person
 * ends has no wait.  Constants, not settings.
 */
#define AXIS_ATTENTION_DISK_WAIT_S     (10 * 60)
#define AXIS_ATTENTION_CLIENT_WAIT_S   (30 * 60)
#define AXIS_ATTENTION_INDEXER_WAIT_S  (30 * 60)

/* The titles' counts (the heaviest reading) are made only after a job that
 * can change them ended (a search, an import), or this long after the last
 * count when none did. */
#define AXIS_ATTENTION_TITLES_EVERY_S  (10 * 60)

/* A source's statement is given up after this long, waiting for a
 * connection included; the source then keeps what it last gave. */
#define AXIS_ATTENTION_DEADLINE_MS     500

/* More things of one kind than this are one notice with their number. */
#define AXIS_ATTENTION_MAX_OF_A_KIND   5

/* The dashboard's card shows this many notices, the most serious first, and
 * links to the page for the rest. */
#define AXIS_ATTENTION_CARD_ROWS       5

/*
 * The waits of slice N2, constants as the first are.  A media server is
 * checked again by itself, at an interval that grows while nothing plays; a
 * worker says it is alive every few seconds while it runs, and one that was
 * stopped on purpose is not an outage for a while.
 */
#define AXIS_ATTENTION_SERVER_WAIT_S   (30 * 60)
#define AXIS_ATTENTION_WORKER_WAIT_S   (10 * 60)

/* A worker not heard from for this long is taken as switched off for good,
 * and is no notice any more. */
#define AXIS_ATTENTION_WORKER_GONE_S   (24 * 3600)

/* A finished download that cannot be imported is a notice once it has been
 * so for this long: a client that is still moving the file to its place ends
 * the state by itself.  A conversion run that is ready is one this long after
 * it was planned: a person who plans a run usually starts it at once. */
#define AXIS_ATTENTION_STUCK_WAIT_S    (10 * 60)
#define AXIS_ATTENTION_READY_WAIT_S    (10 * 60)

/* Whatever ended, two counts of a module's titles are at least this far
 * apart. */
#define AXIS_ATTENTION_TITLES_FLOOR_S  (5 * 60)

/**
 * AxisAttentionLevel:
 * @AXIS_ATTENTION_LOOK: "Worth a look": something is degraded and the rest
 *   still works
 * @AXIS_ATTENTION_WAITING: "Waiting for you": Axis stopped where only a
 *   person can decide
 * @AXIS_ATTENTION_BROKEN: "Not working": something is broken and media
 *   cannot arrive or be stored until it is mended
 *
 * A higher value is more serious (as stored in attention_dismissal.severity).
 */
typedef enum
{
	AXIS_ATTENTION_LOOK    = 1,
	AXIS_ATTENTION_WAITING = 2,
	AXIS_ATTENTION_BROKEN  = 3
} AxisAttentionLevel;

/**
 * AxisAttentionItem:
 * @kind: a fixed name ("volume.offline", "import.blocked"): lower-case
 *   letters, digits, '.' and '_', at most 63 bytes
 * @subject: the id of the one thing it is about, as text ("3"), or "" for a
 *   count of many; never a name or a path; at most 64 bytes
 * @severity: its level
 * @count: how many things it stands for (1 for one thing)
 * @newest: a number that only rises when the thing happens again: when the
 *   state began (seconds since 1970), or the highest id among the things
 *   counted; 0 when the source has none
 * @text: the sentence, up to the name when it has one
 * @name: (nullable): a name inside the sentence, drawn in its own <bdi>
 * @tail: (nullable): the rest of the sentence after the name
 * @href: the page where it is dealt with (a path on this site)
 * @page: the words of the link to it ("Open the disk")
 * @since_us: when it began (microseconds since 1970), 0 when not known
 * @admin_only: only an administrator is shown it (set by
 *   axis_attention_item_new(); every item of this slice keeps it)
 * @not_known: not a notice but a source's answer "not known yet" (made by
 *   axis_attention_not_known() alone); the board takes it out of the list
 * @any_subject: with @not_known: about whatever subject
 */
typedef struct
{
	gchar              *kind;
	gchar              *subject;
	AxisAttentionLevel  severity;
	guint64             count;
	gint64              newest;
	gchar              *text;
	gchar              *name;
	gchar              *tail;
	gchar              *href;
	gchar              *page;
	gint64              since_us;
	gboolean            admin_only;
	gboolean            not_known;
	gboolean            any_subject;
} AxisAttentionItem;

/* A new item, an administrator's, with no words yet.  @subject: (nullable). */
AxisAttentionItem *
axis_attention_item_new(
	const gchar        *kind,
	const gchar        *subject,
	AxisAttentionLevel  severity,
	guint64             count,
	gint64              newest
);

/* Sets the sentence: @text, then @name (nullable) in its own <bdi>, then
 * @tail (nullable). */
void
axis_attention_item_say(
	AxisAttentionItem *item,
	const gchar       *text,
	const gchar       *name,
	const gchar       *tail
);

/* Sets where it is dealt with and the link's words. */
void
axis_attention_item_link(
	AxisAttentionItem *item,
	const gchar       *href,
	const gchar       *page
);

void
axis_attention_item_free(AxisAttentionItem *item);

/**
 * axis_attention_not_known:
 * @items: the list a source was given
 * @kind: a kind of the source's
 * @subject: (nullable): the one thing it cannot speak of, as an item's
 *   subject is written; %NULL for every thing of @kind
 *
 * A source's answer "not known yet" for a kind: it cannot say in this
 * reading whether there is such a notice (a disk this daemon has not checked
 * yet, a count that is not finished, a download whose disk is away).  Giving
 * no item says "there is none", which ends the notice and with it every
 * user's dismissal of it; this says neither.  The board then keeps the items
 * of @kind (about @subject) the source last gave, as they are, and no
 * dismissal of them ends: at a start, when there are no last items, the
 * dismissals read from the database wait for a reading that knows.
 *
 * An item of the same kind and subject given in the same reading is the
 * newer and is taken instead.
 */
void
axis_attention_not_known(
	GPtrArray   *items,
	const gchar *kind,
	const gchar *subject
);

/* "25 minutes", "1 hour", "3 days": how long @us microseconds are, in the
 * largest unit that fits.  Transfer full. */
gchar *
axis_attention_duration(gint64 us);

/**
 * AxisAttentionReading:
 * @now_us: the time of the reading (microseconds since 1970); a source
 *   judges its waits by this, never by the clock
 * @jobs_ended: (element-type utf8 utf8): the kinds of the jobs that ended
 *   since the reading before (a set)
 * @deadline_ms: how long a source's statement may take in all
 *   (%AXIS_ATTENTION_DEADLINE_MS): pass it to axis_db_pool_query_bounded()
 *
 * One reading of the board, as a source is given it.
 */
typedef struct
{
	gint64      now_us;
	GHashTable *jobs_ended;
	guint       deadline_ms;
} AxisAttentionReading;

/* Whether a job of exactly @kind ("movies.search") ended since the reading
 * before. */
gboolean
axis_attention_reading_job_ended(
	const AxisAttentionReading *reading,
	const gchar                *kind
);

/**
 * AxisAttentionTitles:
 * @known: counted at least once
 * @dirty: a job that can change the count ended since it was counted
 * @read_us: the reading's time when it was counted
 * @count: how many titles need attention for a reason of their own
 * @newest: the highest id among them
 * @top: the most common reason among them (an AxisAcquireAttention)
 * @n_reasons: how many different reasons they have
 *
 * What a source that counts titles (Movies, TV) keeps between readings: the
 * count is the heaviest reading, so it is not made at every reading, and the
 * source gives what it last counted in between (see
 * axis_attention_titles_due()).  Zeroed to begin with; touched by the source
 * alone (which never runs twice at once).
 */
typedef struct
{
	gboolean known;
	gboolean dirty;
	gint64   read_us;
	guint64  count;
	gint64   newest;
	guint    top;
	guint    n_reasons;
} AxisAttentionTitles;

/**
 * axis_attention_titles_due:
 * @kinds: the job kinds whose end can change the count, by their exact
 *   names, ended by %NULL: the searches and the imports, never a job that
 *   runs every minute whatever happened (one that follows the downloads, or
 *   plans the searches)
 *
 * Whether the titles are to be counted in @reading.  The first time, always.
 * After that never sooner than %AXIS_ATTENTION_TITLES_FLOOR_S after the count
 * before, whatever ended; then when a job of @kinds ended since (remembered
 * until a count is made), or %AXIS_ATTENTION_TITLES_EVERY_S after the count
 * before when none did.
 */
gboolean
axis_attention_titles_due(
	AxisAttentionTitles        *titles,
	const AxisAttentionReading *reading,
	const gchar *const         *kinds
);

/**
 * AxisAttentionSourceFunc:
 * @reading: the reading
 * @items: (element-type AxisAttentionItem): add the source's items here
 *   (it takes them)
 * @user_data: (closure): as given to axis_web_add_attention_source()
 * @error: why it could not be read
 *
 * Runs on a worker thread, never the main loop, and never twice at once: it
 * may block on the database (one small statement, none over the index of
 * files), and on nothing else: a stop (axis_web_stop()) ends the wait on the
 * database the source is in at once and makes its later statements fail
 * without waiting (AxisDbInterrupt, attached to the reading's thread), then
 * waits for the source to return.  Must not touch anything that belongs to
 * the main loop.
 *
 * Returns: %FALSE when it could not be read: the board then keeps the items
 *   the source last gave and says since when they are unchanged.  A source
 *   that can be read but cannot yet say of one kind answers %TRUE and
 *   axis_attention_not_known() for that kind.
 */
typedef gboolean (*AxisAttentionSourceFunc)(
	const AxisAttentionReading  *reading,
	GPtrArray                   *items,
	gpointer                     user_data,
	GError                     **error
);

/**
 * axis_web_add_attention_source:
 * @self: the shell
 * @name: what the source is about, for the page's line when it cannot be
 *   read ("the disks")
 * @func: the source
 * @user_data: for @func; must outlive the shell's requests and
 *   axis_web_stop()
 *
 * One module's notices.  Call from register_routes: the source goes with its
 * module (a module switched off has no notice).
 */
void
axis_web_add_attention_source(
	AxisWeb                 *self,
	const gchar             *name,
	AxisAttentionSourceFunc  func,
	gpointer                 user_data
);

/**
 * axis_web_attention_start:
 * @self: the shell
 * @pool: (nullable): the database the dismissals are kept in; %NULL keeps
 *   them in memory only (tests)
 *
 * Starts the readings: a first one at once, then as the header says.  Until
 * it is called the board is empty and nothing is read (the bell shows
 * nothing waiting).  axis_web_stop() ends them: a reading in flight is called
 * off (its wait on the database ends at once, it runs no further source, and
 * what it gave is thrown away) and waited for, which takes no longer than a
 * source needs to return from a statement that failed, whether the database
 * answers or not.  Main loop.
 */
void
axis_web_attention_start(
	AxisWeb    *self,
	AxisDbPool *pool
);

/* Something that can change the board happened (a disk's state, a setting, a
 * person's action): it is read again soon.  Main loop; a shell that was not
 * started, or %NULL, ignores it.  The shell calls it itself after every
 * request of a logged-in user that is not a GET. */
void
axis_web_attention_changed(AxisWeb *self);

/* A job of @kind ended.  As axis_web_attention_changed(), and the sources
 * that read only after their module's jobs learn of it. */
void
axis_web_attention_job_ended(
	AxisWeb     *self,
	const gchar *kind
);

/**
 * AxisAttentionPing:
 *
 * A way to say "the board may have changed" that a module can keep beyond
 * the shell's life: a module's object is often freed after the shell, and
 * what it listens to (the job queue, the storage layer) may still call it.
 * Once the shell is freed a ping does nothing.  Main loop only.
 */
typedef struct _AxisAttentionPing AxisAttentionPing;

/* Transfer full; from register_routes. */
AxisAttentionPing *
axis_web_attention_ref_ping(AxisWeb *self);

/* %NULL is ignored. */
void
axis_attention_ping_unref(AxisAttentionPing *ping);

/* As axis_web_attention_changed(); %NULL is ignored. */
void
axis_attention_ping_changed(AxisAttentionPing *ping);

/* As axis_web_attention_job_ended(); %NULL is ignored. */
void
axis_attention_ping_job_ended(
	AxisAttentionPing *ping,
	const gchar       *kind
);

/* ── For tests ──────────────────────────────────────────────────── */

/**
 * AxisAttentionTuning:
 * @min_gap_ms: instead of %AXIS_ATTENTION_MIN_GAP_S (0: keep)
 * @period_ms: instead of %AXIS_ATTENTION_PERIOD_S (0: keep)
 * @clock_offset_us: added to the clock for every reading's @now_us
 * @deadline_ms: instead of %AXIS_ATTENTION_DEADLINE_MS (0: keep)
 */
typedef struct
{
	guint  min_gap_ms;
	guint  period_ms;
	gint64 clock_offset_us;
	guint  deadline_ms;
} AxisAttentionTuning;

/* Before or after axis_web_attention_start(). */
void
axis_web_attention_set_tuning(
	AxisWeb                   *self,
	const AxisAttentionTuning *tuning
);

/* How many readings have finished. */
guint
axis_web_attention_get_readings(AxisWeb *self);

/* How many times the event was published (whether or not anyone followed
 * the topic is the hub's to say: this counts the board's own decisions to
 * tell). */
guint
axis_web_attention_get_changes(AxisWeb *self);

G_END_DECLS

#endif /* AXIS_WEB_ATTENTION_H */
