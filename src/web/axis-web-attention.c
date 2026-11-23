/*
 * axis-web-attention.c - what needs a person: the board the sources fill,
 * what each user dismissed, the bell in the header and the page
 * "Needs attention"
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * See web/axis-web-attention.h for the whole of it.  In short:
 *
 *   the board       what every source last gave, in memory.  Read again on a
 *                   worker thread: every source of a switched-on module, one
 *                   after another.  Only the main loop touches the board; the
 *                   thread works on its own lists and hands them over.
 *   the dismissals  what each user dismissed, in memory, read from
 *                   attention_dismissal with the first reading and kept in
 *                   step when a person dismisses or shows again.
 *   the bell        a link to /attention with the number of notices the user
 *                   has not dismissed: a walk over the board, no statement.
 *   the page        the notices by level, each with its link and "Dismiss",
 *                   and what the user dismissed with "Show again".
 *
 * Everything drawn here is drawn for one user: an item reaches a list only
 * through may_see(), and the routes are administrators' (the bell's own
 * request answers anyone else 403 with nothing in it, so that the bell of a
 * page left open by someone whose role was taken away simply goes).
 */

#include "web/axis-web-attention.h"

#include <string.h>

#include "web/axis-web-auth-private.h"
#include "web/axis-web-hub.h"
#include "web/axis-web-private.h"

#define BELL_ID        "attention-bell"
#define LIST_ID        "attention-list"
#define PAGE_PATH      "/attention"
#define BELL_PATH      "/attention/bell"
#define DISMISS_PATH   "/attention/dismiss"
#define RESTORE_PATH   "/attention/restore"

#define KIND_MAX       63
#define SUBJECT_MAX    64

/* Posts waiting for their thread beyond this are answered 503. */
#define MAX_PENDING    8

typedef struct
{
	AxisModuleSwitch        *sw;	/* its module's flag, or %NULL */
	gchar                   *name;
	AxisAttentionSourceFunc  func;
	gpointer                 data;
	GPtrArray               *items;	/* of AxisAttentionItem*: what it last gave */
	gint64                   read_us;	/* when that was read (real time); 0: never */
	gboolean                 failed;	/* its last reading failed */
} Source;

typedef struct
{
	gint64   user_id;
	gchar   *kind;
	gchar   *subject;
	gint     severity;
	guint64  count;
	gint64   newest;
} Dismissal;

/* What a module keeps to say the board may have changed: it outlives the
 * board, and does nothing once the board is gone. */
struct _AxisAttentionPing
{
	guint             refs;
	AxisWebAttention *board;	/* %NULL once it is freed */
};

struct _AxisWebAttention
{
	AxisWeb    *web;
	AxisAttentionPing *ping;
	AxisDbPool *pool;		/* nullable: the dismissals are in memory only */
	AxisDbInterrupt *interrupt;	/* raised by a stop: ends the threads' waits on the database */
	GPtrArray  *sources;		/* of Source* */
	GHashTable *dismissals;		/* key -> Dismissal* */
	gboolean    dismissals_loaded;
	GHashTable *touched;		/* keys a person dismissed or restored before the rows were read */
	GPtrArray  *ended;		/* of Dismissal*: rows of notices that ended, to delete */
	GHashTable *jobs_ended;		/* a set of kinds, since the last reading began */
	gboolean    started;
	gboolean    stopping;
	gboolean    reading;		/* a reading is on its thread */
	gboolean    stale;
	guint       pending;		/* posts on their threads */
	guint       soon_id;
	guint       period_id;
	gint64      last_read_us;	/* monotonic; 0: never */
	guint       min_gap_ms;
	guint       period_ms;
	guint       deadline_ms;
	gint64      clock_offset_us;
	guint       readings;
	guint       changes;
	gchar      *signature;		/* of the board as last told */
};

/* Adds @child to @parent and drops the caller's reference. */
static void
add_owned(
	gpointer parent,
	gpointer child
){
	htmx_node_add_child(HTMX_NODE(parent), HTMX_NODE(child));
	g_object_unref(child);
}

/* ── Items ──────────────────────────────────────────────────────── */

AxisAttentionItem *
axis_attention_item_new(
	const gchar        *kind,
	const gchar        *subject,
	AxisAttentionLevel  severity,
	guint64             count,
	gint64              newest
){
	AxisAttentionItem *item = g_new0(AxisAttentionItem, 1);

	item->kind = g_strdup(kind);
	item->subject = g_strdup(subject != NULL ? subject : "");
	item->severity = severity;
	item->count = count;
	item->newest = MAX(newest, 0);
	/* Unless its source says otherwise, an item is an administrator's. */
	item->admin_only = TRUE;
	return item;
}

void
axis_attention_item_say(
	AxisAttentionItem *item,
	const gchar       *text,
	const gchar       *name,
	const gchar       *tail
){
	g_return_if_fail(item != NULL && text != NULL);

	g_free(item->text);
	g_free(item->name);
	g_free(item->tail);
	item->text = g_strdup(text);
	item->name = g_strdup(name);
	item->tail = g_strdup(tail);
}

void
axis_attention_item_link(
	AxisAttentionItem *item,
	const gchar       *href,
	const gchar       *page
){
	g_return_if_fail(item != NULL && href != NULL && page != NULL);

	g_free(item->href);
	g_free(item->page);
	item->href = g_strdup(href);
	item->page = g_strdup(page);
}

void
axis_attention_item_free(AxisAttentionItem *item)
{
	if (item == NULL)
		return;
	g_free(item->kind);
	g_free(item->subject);
	g_free(item->text);
	g_free(item->name);
	g_free(item->tail);
	g_free(item->href);
	g_free(item->page);
	g_free(item);
}

static gboolean
kind_is_valid(const gchar *kind);

static gboolean
subject_is_valid(const gchar *subject);

void
axis_attention_not_known(
	GPtrArray   *items,
	const gchar *kind,
	const gchar *subject
){
	AxisAttentionItem *item;

	g_return_if_fail(items != NULL && kind_is_valid(kind) && (subject == NULL || subject_is_valid(subject)));

	item = axis_attention_item_new(kind, subject, AXIS_ATTENTION_LOOK, 0, 0);
	item->not_known = TRUE;
	item->any_subject = subject == NULL;
	g_ptr_array_add(items, item);
}

/* What "not known yet" is kept under: the kind for every subject, or the
 * kind and one subject (neither holds a '/').  Transfer full. */
static gchar *
not_known_key(
	const gchar *kind,
	const gchar *subject	/* nullable: every subject */
){
	return subject == NULL ? g_strdup(kind) : g_strdup_printf("%s/%s", kind, subject);
}

static gboolean
is_not_known(
	GHashTable  *not_known,
	const gchar *kind,
	const gchar *subject
){
	g_autofree gchar *one = not_known_key(kind, subject);

	return g_hash_table_contains(not_known, kind) || g_hash_table_contains(not_known, one);
}

gchar *
axis_attention_duration(gint64 us)
{
	gint64 minutes = MAX(us, 0) / (60 * G_USEC_PER_SEC);

	if (minutes < 1)
		return g_strdup("less than a minute");
	if (minutes < 120)
		return g_strdup_printf("%" G_GINT64_FORMAT " %s", minutes, minutes == 1 ? "minute" : "minutes");
	if (minutes < 48 * 60)
		return g_strdup_printf("%" G_GINT64_FORMAT " hours", minutes / 60);
	return g_strdup_printf("%" G_GINT64_FORMAT " days", minutes / (24 * 60));
}

gboolean
axis_attention_reading_job_ended(
	const AxisAttentionReading *reading,
	const gchar                *kind
){
	g_return_val_if_fail(reading != NULL && kind != NULL, FALSE);

	return reading->jobs_ended != NULL && g_hash_table_contains(reading->jobs_ended, kind);
}

gboolean
axis_attention_titles_due(
	AxisAttentionTitles        *titles,
	const AxisAttentionReading *reading,
	const gchar *const         *kinds
){
	gint64 age;
	guint i;

	g_return_val_if_fail(titles != NULL && reading != NULL && kinds != NULL, TRUE);

	/* Remembered until a count is made: one that is put off by the floor,
	 * or fails, keeps it. */
	for (i = 0; kinds[i] != NULL; i++) {
		if (axis_attention_reading_job_ended(reading, kinds[i]))
			titles->dirty = TRUE;
	}
	if (!titles->known)
		return TRUE;
	age = reading->now_us - titles->read_us;
	/* The clock went back: count, and begin from there. */
	if (age < 0)
		return TRUE;
	if (age < (gint64)AXIS_ATTENTION_TITLES_FLOOR_S * G_USEC_PER_SEC)
		return FALSE;
	return titles->dirty || age >= (gint64)AXIS_ATTENTION_TITLES_EVERY_S * G_USEC_PER_SEC;
}

/* attention_dismissal's own rule for a kind: '^[a-z][a-z0-9_.]{0,62}$'. */
static gboolean
kind_is_valid(const gchar *kind)
{
	gsize i;

	if (kind == NULL || !g_ascii_islower(kind[0]))
		return FALSE;
	for (i = 1; kind[i] != '\0'; i++) {
		if (i >= KIND_MAX || !(g_ascii_islower(kind[i]) || g_ascii_isdigit(kind[i]) || kind[i] == '_' || kind[i] == '.'))
			return FALSE;
	}
	return TRUE;
}

/* An id as text, or nothing: never a name or a path. */
static gboolean
subject_is_valid(const gchar *subject)
{
	gsize i;

	if (subject == NULL)
		return FALSE;
	for (i = 0; subject[i] != '\0'; i++) {
		if (i >= SUBJECT_MAX || !(g_ascii_isalnum(subject[i]) || subject[i] == '_' || subject[i] == '.' ||
		    subject[i] == '-'))
			return FALSE;
	}
	return TRUE;
}

static gboolean
level_is_valid(gint64 level)
{
	return level >= AXIS_ATTENTION_LOOK && level <= AXIS_ATTENTION_BROKEN;
}

/* Whether a source's item can be put on the board: what a page draws and a
 * dismissal stores must hold. */
static gboolean
item_is_sound(const AxisAttentionItem *item)
{
	return kind_is_valid(item->kind) && subject_is_valid(item->subject) && level_is_valid(item->severity) &&
		item->count <= (guint64)G_MAXINT64 && item->newest >= 0 && item->text != NULL && item->page != NULL &&
		item->href != NULL && item->href[0] == '/' && item->href[1] != '/' && strchr(item->href, '\\') == NULL;
}

/* ── Sources ────────────────────────────────────────────────────── */

static void
source_free(Source *source)
{
	axis_module_switch_unref(source->sw);
	g_free(source->name);
	g_ptr_array_unref(source->items);
	g_free(source);
}

static gboolean
source_is_live(const Source *source)
{
	return source->sw == NULL || axis_module_switch_is_on(source->sw);
}

/* ── Dismissals ─────────────────────────────────────────────────── */

static void
dismissal_free(Dismissal *dismissal)
{
	if (dismissal == NULL)
		return;
	g_free(dismissal->kind);
	g_free(dismissal->subject);
	g_free(dismissal);
}

static Dismissal *
dismissal_copy(const Dismissal *from)
{
	Dismissal *copy = g_new0(Dismissal, 1);

	*copy = *from;
	copy->kind = g_strdup(from->kind);
	copy->subject = g_strdup(from->subject);
	return copy;
}

/* Neither a user's id nor a kind holds a '/', so a key is one thing's. */
static gchar *
dismissal_key(
	gint64       user_id,
	const gchar *kind,
	const gchar *subject
){
	return g_strdup_printf("%" G_GINT64_FORMAT "/%s/%s", user_id, kind, subject);
}

static const Dismissal *
dismissal_of(
	AxisWebAttention        *self,
	gint64                   user_id,
	const AxisAttentionItem *item
){
	g_autofree gchar *key = dismissal_key(user_id, item->kind, item->subject);

	return g_hash_table_lookup(self->dismissals, key);
}

/*
 * The whole rule of a dismissal: the item stays out of the user's count and
 * list while its level, its number and its "newest" are all no higher than
 * what the user dismissed.  So it is back when the thing happens again (a
 * later "newest"), when there are more of them, or when it gets more
 * serious; never because time passed, and not when things got better.
 */
static gboolean
dismissal_hides(
	const Dismissal         *dismissal,
	const AxisAttentionItem *item
){
	return dismissal != NULL && (gint)item->severity <= dismissal->severity && item->count <= dismissal->count &&
		item->newest <= dismissal->newest;
}

/* ── Who sees what ──────────────────────────────────────────────── */

static gboolean
is_admin(const AxisAuthState *state)
{
	return state != NULL && state->user_id != 0 && state->role == AXIS_USER_ROLE_ADMIN;
}

/*
 * Whether the user of @state may have a notice at all, which is whether
 * they are shown the bell.  In this slice every item is an administrator's,
 * so only an administrator may.
 */
static gboolean
may_have_notice(const AxisAuthState *state)
{
	return is_admin(state);
}

/* The one gate between the board and a page: every list a page draws is
 * filled through it. */
static gboolean
may_see(
	const AxisAuthState     *state,
	const AxisAttentionItem *item
){
	if (state == NULL || state->user_id == 0)
		return FALSE;
	return state->role == AXIS_USER_ROLE_ADMIN || !item->admin_only;
}

static gint
compare_items(
	gconstpointer a,
	gconstpointer b
){
	const AxisAttentionItem *x = *(AxisAttentionItem *const *)a;
	const AxisAttentionItem *y = *(AxisAttentionItem *const *)b;
	gint order;

	/* The most serious first; in a level, the newest first. */
	if (x->severity != y->severity)
		return x->severity > y->severity ? -1 : 1;
	if (x->since_us != y->since_us)
		return x->since_us > y->since_us ? -1 : 1;
	if (x->newest != y->newest)
		return x->newest > y->newest ? -1 : 1;
	order = strcmp(x->kind, y->kind);
	return order != 0 ? order : strcmp(x->subject, y->subject);
}

/*
 * What the user of @state is shown: in @shown the items they have not
 * dismissed, in @dismissed (nullable) those they have; both borrowed from
 * the board, in the page's order.  No statement is run: the board and the
 * dismissals are in memory.
 */
static void
items_for(
	AxisWebAttention    *self,
	const AxisAuthState *state,
	GPtrArray           *shown,
	GPtrArray           *dismissed
){
	guint i;
	guint k;

	if (!may_have_notice(state))
		return;
	for (i = 0; i < self->sources->len; i++) {
		const Source *source = g_ptr_array_index(self->sources, i);

		/* A module switched off has no notice, from this page view on. */
		if (!source_is_live(source))
			continue;
		for (k = 0; k < source->items->len; k++) {
			AxisAttentionItem *item = g_ptr_array_index(source->items, k);

			if (!may_see(state, item))
				continue;
			if (dismissal_hides(dismissal_of(self, state->user_id, item), item)) {
				if (dismissed != NULL)
					g_ptr_array_add(dismissed, item);
			} else {
				g_ptr_array_add(shown, item);
			}
		}
	}
	g_ptr_array_sort(shown, compare_items);
	if (dismissed != NULL)
		g_ptr_array_sort(dismissed, compare_items);
}

/* ── Telling the pages ──────────────────────────────────────────── */

static gboolean
is_live(const AxisWebAttention *self)
{
	return self->started && !self->stopping;
}

/* The event carries nothing of the board: each page asks for its own. */
static void
tell(AxisWebAttention *self)
{
	AxisWebHub *hub = axis_web_get_hub(self->web);

	self->changes++;
	if (hub != NULL && axis_web_hub_count_subscribers(hub, AXIS_ATTENTION_TOPIC) > 0)
		axis_web_hub_publish(hub, AXIS_ATTENTION_TOPIC, AXIS_ATTENTION_EVENT, "changed");
}

static gint
compare_text(
	gconstpointer a,
	gconstpointer b
){
	return strcmp(*(const gchar *const *)a, *(const gchar *const *)b);
}

/* The board as one text: what a change of it is judged by (an item came,
 * went, or its level, number or "newest" changed; never its words). */
static gchar *
board_signature(AxisWebAttention *self)
{
	g_autoptr(GPtrArray) lines = g_ptr_array_new_with_free_func(g_free);
	guint i;
	guint k;

	for (i = 0; i < self->sources->len; i++) {
		const Source *source = g_ptr_array_index(self->sources, i);

		if (!source_is_live(source))
			continue;
		for (k = 0; k < source->items->len; k++) {
			const AxisAttentionItem *item = g_ptr_array_index(source->items, k);

			g_ptr_array_add(lines, g_strdup_printf("%s/%s/%d/%" G_GUINT64_FORMAT "/%" G_GINT64_FORMAT, item->kind,
				item->subject, (gint)item->severity, item->count, item->newest));
		}
	}
	g_ptr_array_sort(lines, compare_text);
	g_ptr_array_add(lines, NULL);
	return g_strjoinv("\n", (gchar **)lines->pdata);
}

/* ── A reading ──────────────────────────────────────────────────── */

typedef struct
{
	Source    *source;	/* borrowed: the sources live as long as the board */
	GPtrArray *items;
	gboolean   ok;
	GError    *error;
} Run;

typedef struct
{
	AxisWebAttention     *owner;
	AxisDbPool           *pool;
	AxisDbInterrupt      *interrupt;	/* borrowed: the board waits for the thread */
	AxisAttentionReading  reading;
	GArray               *runs;		/* of Run */
	gboolean              load;		/* read the dismissals */
	gboolean              loaded;
	GPtrArray            *dismissals;	/* of Dismissal*: as read */
	GPtrArray            *forget;		/* of Dismissal*: rows to delete */
} Reading;

static void
reading_free(Reading *reading)
{
	guint i;

	for (i = 0; i < reading->runs->len; i++) {
		Run *run = &g_array_index(reading->runs, Run, i);

		g_clear_pointer(&run->items, g_ptr_array_unref);
		g_clear_error(&run->error);
	}
	g_array_unref(reading->runs);
	g_clear_pointer(&reading->reading.jobs_ended, g_hash_table_unref);
	g_ptr_array_unref(reading->dismissals);
	g_ptr_array_unref(reading->forget);
	g_free(reading);
}

static void
schedule(AxisWebAttention *self);

/*
 * On the worker thread: nothing of the board is touched.
 *
 * A stop calls the reading off (axis_web_attention_stop()): the wait on the
 * database the thread is in ends at once, the statements it still tries fail
 * without waiting, and no further source is run.  What such a reading gave
 * is thrown away, whole.
 */
static void
reading_thread(
	GTask        *gtask,
	gpointer      source_object,
	gpointer      task_data,
	GCancellable *cancellable
){
	Reading *reading = task_data;
	guint i;

	axis_db_interrupt_attach(reading->interrupt);

	if (reading->load && !axis_db_interrupt_is_raised(reading->interrupt)) {
		g_autoptr(AxisDbResult) result = NULL;
		g_autoptr(GError) error = NULL;

		/* Under the deadline every source's statement has: rows that could
		 * not be read are asked for again by the next reading. */
		if (axis_db_pool_query_bounded(reading->pool, "SELECT user_id, kind, subject, severity, count, newest"
		        " FROM attention_dismissal", reading->reading.deadline_ms, &result, &error)) {
			guint n = axis_db_result_get_n_rows(result);

			for (i = 0; i < n; i++) {
				Dismissal *dismissal = g_new0(Dismissal, 1);
				gint64 severity = 0;
				gint64 count = 0;

				axis_db_result_get_int64(result, i, 0, &dismissal->user_id);
				dismissal->kind = g_strdup(axis_db_result_get_text(result, i, 1));
				dismissal->subject = g_strdup(axis_db_result_get_text(result, i, 2));
				axis_db_result_get_int64(result, i, 3, &severity);
				axis_db_result_get_int64(result, i, 4, &count);
				axis_db_result_get_int64(result, i, 5, &dismissal->newest);
				dismissal->severity = (gint)severity;
				dismissal->count = (guint64)MAX(count, 0);
				if (dismissal->kind == NULL || dismissal->subject == NULL)
					dismissal_free(dismissal);
				else
					g_ptr_array_add(reading->dismissals, dismissal);
			}
			reading->loaded = TRUE;
		} else {
			g_debug("attention: cannot read what was dismissed: %s", error->message);
		}
	}

	for (i = 0; i < reading->runs->len; i++) {
		Run *run = &g_array_index(reading->runs, Run, i);

		/* Called off: the sources after this one are not run. */
		if (axis_db_interrupt_is_raised(reading->interrupt))
			break;
		run->ok = run->source->func(&reading->reading, run->items, run->source->data, &run->error);
	}

	for (i = 0; i < reading->forget->len && !axis_db_interrupt_is_raised(reading->interrupt); i++) {
		const Dismissal *dismissal = g_ptr_array_index(reading->forget, i);
		g_autofree gchar *user = g_strdup_printf("%" G_GINT64_FORMAT, dismissal->user_id);
		g_autofree gchar *severity = g_strdup_printf("%d", dismissal->severity);
		g_autofree gchar *count = g_strdup_printf("%" G_GUINT64_FORMAT, dismissal->count);
		g_autofree gchar *newest = g_strdup_printf("%" G_GINT64_FORMAT, dismissal->newest);
		g_autoptr(GError) error = NULL;
		const gchar *params[6];

		params[0] = user;
		params[1] = dismissal->kind;
		params[2] = dismissal->subject;
		params[3] = severity;
		params[4] = count;
		params[5] = newest;
		/* Only the row as it was when its notice ended: one written since (the
		 * thing happened again and was dismissed again) is another. */
		if (!axis_db_pool_exec(reading->pool, "DELETE FROM attention_dismissal WHERE user_id = $1::bigint"
		        " AND kind = $2::text AND subject = $3::text AND severity = $4::smallint AND count = $5::bigint"
		        " AND newest = $6::bigint", 6, params, NULL, &error))
			g_debug("attention: cannot delete the dismissal of a notice that ended: %s", error->message);
	}

	/* The thread is the pool's again: what runs on it next is not ours. */
	axis_db_interrupt_attach(NULL);
	g_task_return_boolean(gtask, TRUE);
}

/*
 * A dismissal lasts while its notice lasts.  A notice that is not on the
 * board after a whole reading has ended (its cause is gone), and with it go
 * the dismissals of it, every user's: the next time the thing happens it is
 * shown, whatever its number and its "newest" are then.  (A dismissal alone
 * is a high-water mark: twelve movies dismissed, mended, and twelve other,
 * older ones failing later would stay hidden.)  A number that drops without
 * reaching nothing keeps the notice, and so its dismissals.  The rows are
 * deleted by the next reading's thread.
 *
 * A notice whose source answered "not known yet" in this reading
 * (@not_known; axis_attention_not_known()) has not ended, on the board or
 * not: after a start the board holds nothing of a kind until a reading knows
 * it, and the dismissals read from the database wait for that reading.
 */
static void
end_dismissals(
	AxisWebAttention *self,
	GHashTable       *not_known
){
	g_autoptr(GHashTable) on_board = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	GHashTableIter iter;
	gpointer value;
	guint i;
	guint k;

	if (g_hash_table_size(self->dismissals) == 0)
		return;
	for (i = 0; i < self->sources->len; i++) {
		const Source *source = g_ptr_array_index(self->sources, i);

		for (k = 0; k < source->items->len; k++) {
			const AxisAttentionItem *item = g_ptr_array_index(source->items, k);

			g_hash_table_add(on_board, g_strdup_printf("%s/%s", item->kind, item->subject));
		}
	}
	g_hash_table_iter_init(&iter, self->dismissals);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		const Dismissal *dismissal = value;
		g_autofree gchar *thing = g_strdup_printf("%s/%s", dismissal->kind, dismissal->subject);

		if (g_hash_table_contains(on_board, thing) || is_not_known(not_known, dismissal->kind, dismissal->subject))
			continue;
		if (self->pool != NULL)
			g_ptr_array_add(self->ended, dismissal_copy(dismissal));
		g_hash_table_iter_remove(&iter);
	}
	/* Their rows go with the next reading, which is soon. */
	if (self->ended->len > 0)
		self->stale = TRUE;
}

static void
reading_done(
	GObject      *source_object,
	GAsyncResult *result,
	gpointer      user_data
){
	Reading *reading = user_data;
	AxisWebAttention *self = reading->owner;
	/* What the sources of this reading could not say yet, and one source's. */
	g_autoptr(GHashTable) not_known = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_autoptr(GHashTable) kept = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	gboolean all_read = TRUE;
	guint i;

	self->reading = FALSE;
	self->last_read_us = g_get_monotonic_time();
	if (self->stopping) {
		reading_free(reading);
		return;
	}

	if (reading->loaded) {
		/* What a person did while the rows were being read is the newer: a
		 * notice they dismissed then, and one they asked to see again (whose
		 * row, read before it was deleted, must not come back). */
		while (reading->dismissals->len > 0) {
			Dismissal *dismissal = g_ptr_array_steal_index_fast(reading->dismissals, 0);
			gchar *key = dismissal_key(dismissal->user_id, dismissal->kind, dismissal->subject);

			if (self->dismissals_loaded || g_hash_table_contains(self->touched, key) ||
			    g_hash_table_contains(self->dismissals, key)) {
				g_free(key);
				dismissal_free(dismissal);
			} else {
				g_hash_table_insert(self->dismissals, key, dismissal);
			}
		}
		self->dismissals_loaded = TRUE;
		g_hash_table_remove_all(self->touched);
	}

	for (i = 0; i < reading->runs->len; i++) {
		Run *run = &g_array_index(reading->runs, Run, i);
		Source *source = run->source;
		guint k;

		if (!run->ok) {
			/* Its last items stay, and the page says since when. */
			if (!source->failed)
				g_message("attention: %s could not be read: %s", source->name,
					run->error != NULL ? run->error->message : "no reason given");
			source->failed = TRUE;
			all_read = FALSE;
			continue;
		}
		for (k = run->items->len; k > 0; k--) {
			const AxisAttentionItem *item = g_ptr_array_index(run->items, k - 1);

			if (item->not_known) {
				/* Not an item: what the source cannot say yet. */
				if (kind_is_valid(item->kind) && subject_is_valid(item->subject)) {
					gchar *key = not_known_key(item->kind, item->any_subject ? NULL : item->subject);

					g_hash_table_add(not_known, g_strdup(key));
					g_hash_table_add(kept, key);
				} else {
					g_critical("attention: %s said \"not known yet\" of a kind that is not well formed", source->name);
				}
				g_ptr_array_remove_index(run->items, k - 1);
			} else if (!item_is_sound(item)) {
				g_critical("attention: dropping an item of %s that is not well formed (kind '%s')", source->name,
					item->kind != NULL ? item->kind : "(null)");
				g_ptr_array_remove_index(run->items, k - 1);
			}
		}
		/* What it could not say yet stays as it last gave it, unless it gave
		 * the same thing anew. */
		for (k = 0; g_hash_table_size(kept) > 0 && k < source->items->len; k++) {
			AxisAttentionItem *old = g_ptr_array_index(source->items, k);
			gboolean given = FALSE;
			guint m;

			if (!is_not_known(kept, old->kind, old->subject))
				continue;
			for (m = 0; m < run->items->len && !given; m++) {
				const AxisAttentionItem *item = g_ptr_array_index(run->items, m);

				given = strcmp(item->kind, old->kind) == 0 && strcmp(item->subject, old->subject) == 0;
			}
			if (given)
				continue;
			source->items->pdata[k] = NULL;
			g_ptr_array_add(run->items, old);
		}
		g_hash_table_remove_all(kept);
		g_ptr_array_unref(source->items);
		source->items = g_steal_pointer(&run->items);
		source->read_us = g_get_real_time();
		source->failed = FALSE;
	}
	/* A source that was not read (its module is off) has nothing. */
	for (i = 0; i < self->sources->len; i++) {
		Source *source = g_ptr_array_index(self->sources, i);

		if (!source_is_live(source)) {
			g_ptr_array_set_size(source->items, 0);
			source->read_us = 0;
			source->failed = FALSE;
		}
	}
	self->readings++;
	/* Only a whole reading says that a notice has ended: a source that could
	 * not be read says nothing of its own, and before the rows of what was
	 * dismissed are read there is nothing to end. */
	if (all_read && (self->pool == NULL || self->dismissals_loaded))
		end_dismissals(self, not_known);

	{
		gchar *signature = board_signature(self);

		if (g_strcmp0(signature, self->signature) != 0) {
			g_free(self->signature);
			self->signature = signature;
			tell(self);
		} else {
			g_free(signature);
		}
	}
	reading_free(reading);
	schedule(self);
}

static void
begin_reading(AxisWebAttention *self)
{
	Reading *reading = g_new0(Reading, 1);
	GTask *gtask;
	guint i;

	self->reading = TRUE;
	self->stale = FALSE;

	reading->owner = self;
	reading->pool = self->pool;
	reading->interrupt = self->interrupt;
	reading->reading.now_us = g_get_real_time() + self->clock_offset_us;
	reading->reading.deadline_ms = self->deadline_ms;
	reading->reading.jobs_ended = self->jobs_ended;
	self->jobs_ended = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	reading->runs = g_array_new(FALSE, TRUE, sizeof(Run));
	reading->dismissals = g_ptr_array_new_with_free_func((GDestroyNotify)dismissal_free);
	/* The rows of the notices the reading before found ended. */
	reading->forget = self->ended;
	self->ended = g_ptr_array_new_with_free_func((GDestroyNotify)dismissal_free);
	reading->load = self->pool != NULL && !self->dismissals_loaded;

	for (i = 0; i < self->sources->len; i++) {
		Source *source = g_ptr_array_index(self->sources, i);
		Run run;

		if (!source_is_live(source))
			continue;
		memset(&run, 0, sizeof(run));
		run.source = source;
		run.items = g_ptr_array_new_with_free_func((GDestroyNotify)axis_attention_item_free);
		g_array_append_val(reading->runs, run);
	}

	gtask = g_task_new(NULL, NULL, reading_done, reading);
	g_task_set_task_data(gtask, reading, NULL);
	g_task_run_in_thread(gtask, reading_thread);
	g_object_unref(gtask);
}

static gboolean
on_soon(gpointer data)
{
	AxisWebAttention *self = data;

	self->soon_id = 0;
	if (is_live(self) && !self->reading)
		begin_reading(self);
	return G_SOURCE_REMOVE;
}

/* Arms the next reading of a stale board: at once the first time, else no
 * sooner than the gap after the last one finished.  A burst of events is one
 * reading. */
static void
schedule(AxisWebAttention *self)
{
	gint64 wait_ms = 0;

	if (!is_live(self) || !self->stale || self->reading || self->soon_id != 0)
		return;
	if (self->last_read_us != 0)
		wait_ms = (self->last_read_us + (gint64)self->min_gap_ms * 1000 - g_get_monotonic_time()) / 1000;
	self->soon_id = g_timeout_add((guint)CLAMP(wait_ms, 0, G_MAXINT), on_soon, self);
}

static gboolean
on_period(gpointer data)
{
	AxisWebAttention *self = data;

	/* The waits are about time passing, and an event can be missed. */
	self->stale = TRUE;
	schedule(self);
	return G_SOURCE_CONTINUE;
}

/* ── Lifetime ───────────────────────────────────────────────────── */

AxisWebAttention *
axis_web_attention_new(AxisWeb *web)
{
	AxisWebAttention *self = g_new0(AxisWebAttention, 1);

	self->web = web;
	self->ping = g_new0(AxisAttentionPing, 1);
	self->ping->refs = 1;
	self->ping->board = self;
	self->interrupt = axis_db_interrupt_new();
	self->sources =g_ptr_array_new_with_free_func((GDestroyNotify)source_free);
	self->dismissals = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)dismissal_free);
	self->touched = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	self->ended = g_ptr_array_new_with_free_func((GDestroyNotify)dismissal_free);
	self->jobs_ended = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	self->min_gap_ms = AXIS_ATTENTION_MIN_GAP_S * 1000;
	self->period_ms = AXIS_ATTENTION_PERIOD_S * 1000;
	self->deadline_ms = AXIS_ATTENTION_DEADLINE_MS;
	return self;
}

void
axis_web_attention_stop(AxisWebAttention *self)
{
	if (self == NULL)
		return;

	self->stopping = TRUE;
	if (self->soon_id != 0)
		g_source_remove(self->soon_id);
	self->soon_id = 0;
	if (self->period_id != 0)
		g_source_remove(self->period_id);
	self->period_id = 0;
	/*
	 * A reading on its thread runs the modules' sources: it is waited for
	 * before they are stopped; and every post is answered.  Neither wait may
	 * depend on the database answering (a reading is twenty-odd statements,
	 * each given up after the reading's deadline: against a database that
	 * has gone silent that was eleven seconds and more, and a post has no
	 * deadline of its own), so the threads' work is called off first: the
	 * wait on the database a thread is in ends at once, whatever it is (for
	 * a connection of the pool, for a connection attempt, for a statement's
	 * answer), what it still tries fails without waiting, and the reading
	 * runs no further source.  What is left to wait for is what a source
	 * does between two database calls, which is nothing that blocks
	 * (AxisAttentionSourceFunc).  A reading called off is thrown away by
	 * reading_done(); a post called off is answered with its error.
	 */
	axis_db_interrupt_raise(self->interrupt);
	while (self->reading || self->pending > 0)
		g_main_context_iteration(NULL, TRUE);
}

void
axis_web_attention_free(AxisWebAttention *self)
{
	if (self == NULL)
		return;

	axis_web_attention_stop(self);
	/* The pings the modules keep do nothing from here on. */
	self->ping->board = NULL;
	axis_attention_ping_unref(self->ping);
	/* No thread has it attached: the stop waited for every one. */
	axis_db_interrupt_free(self->interrupt);
	g_ptr_array_unref(self->sources);
	g_hash_table_unref(self->dismissals);
	g_hash_table_unref(self->touched);
	g_ptr_array_unref(self->ended);
	g_hash_table_unref(self->jobs_ended);
	g_free(self->signature);
	g_free(self);
}

void
axis_web_attention_add_source(
	AxisWebAttention        *self,
	AxisModuleSwitch        *sw,
	const gchar             *name,
	AxisAttentionSourceFunc  func,
	gpointer                 user_data
){
	Source *source = g_new0(Source, 1);

	source->sw = sw;
	source->name = g_strdup(name);
	source->func = func;
	source->data = user_data;
	source->items = g_ptr_array_new_with_free_func((GDestroyNotify)axis_attention_item_free);
	g_ptr_array_add(self->sources, source);
}

void
axis_web_attention_start(
	AxisWeb    *web,
	AxisDbPool *pool
){
	AxisWebAttention *self;

	g_return_if_fail(web != NULL);

	self = axis_web_get_attention(web);
	if (self->started || self->stopping)
		return;
	self->started = TRUE;
	self->pool = pool;
	self->period_id = g_timeout_add(self->period_ms, on_period, self);
	self->stale = TRUE;
	schedule(self);
}

/* The board is read again soon; @kind: (nullable): a job of it ended. */
static void
mark_stale(
	AxisWebAttention *self,
	const gchar      *kind
){
	/* What this board is told of, the dashboard's figures are read again
	 * for (axis-web-dashboard.c). */
	if (self != NULL && self->web != NULL)
		axis_web_dashboard_parts_changed(axis_web_get_dashboard_parts(self->web));
	if (self == NULL || !is_live(self))
		return;
	if (kind != NULL)
		g_hash_table_add(self->jobs_ended, g_strdup(kind));
	self->stale = TRUE;
	schedule(self);
}

void
axis_web_attention_changed(AxisWeb *web)
{
	mark_stale(web != NULL ? axis_web_get_attention(web) : NULL, NULL);
}

void
axis_web_attention_job_ended(
	AxisWeb     *web,
	const gchar *kind
){
	mark_stale(web != NULL ? axis_web_get_attention(web) : NULL, kind);
}

AxisAttentionPing *
axis_web_attention_ref_ping(AxisWeb *web)
{
	AxisAttentionPing *ping;

	g_return_val_if_fail(web != NULL, NULL);

	ping = axis_web_get_attention(web)->ping;
	ping->refs++;
	return ping;
}

void
axis_attention_ping_unref(AxisAttentionPing *ping)
{
	if (ping != NULL && --ping->refs == 0)
		g_free(ping);
}

void
axis_attention_ping_changed(AxisAttentionPing *ping)
{
	if (ping != NULL)
		mark_stale(ping->board, NULL);
}

void
axis_attention_ping_job_ended(
	AxisAttentionPing *ping,
	const gchar       *kind
){
	if (ping != NULL)
		mark_stale(ping->board, kind);
}

void
axis_web_attention_set_tuning(
	AxisWeb                   *web,
	const AxisAttentionTuning *tuning
){
	AxisWebAttention *self;

	g_return_if_fail(web != NULL && tuning != NULL);

	self = axis_web_get_attention(web);
	if (tuning->min_gap_ms != 0)
		self->min_gap_ms = tuning->min_gap_ms;
	if (tuning->period_ms != 0 && tuning->period_ms != self->period_ms) {
		self->period_ms = tuning->period_ms;
		if (self->period_id != 0) {
			g_source_remove(self->period_id);
			self->period_id = g_timeout_add(self->period_ms, on_period, self);
		}
	}
	if (tuning->deadline_ms != 0)
		self->deadline_ms = tuning->deadline_ms;
	self->clock_offset_us = tuning->clock_offset_us;
}

guint
axis_web_attention_get_readings(AxisWeb *web)
{
	g_return_val_if_fail(web != NULL, 0);
	return axis_web_get_attention(web)->readings;
}

guint
axis_web_attention_get_changes(AxisWeb *web)
{
	g_return_val_if_fail(web != NULL, 0);
	return axis_web_get_attention(web)->changes;
}

/* ── The bell ───────────────────────────────────────────────────── */

/* "3 things need attention, 1 not working": the whole of what the bell
 * shows, for a screen reader; the level is in the words, not only in the
 * badge's colour.  No full stop: it is a name, not a sentence. */
static gchar *
bell_label(
	guint shown,
	guint broken
){
	g_autofree gchar *count = NULL;

	if (shown == 0)
		return g_strdup("Nothing needs attention");
	count = shown == 1 ? g_strdup("1 thing needs attention") : g_strdup_printf("%u things need attention", shown);
	if (broken == 0)
		return g_steal_pointer(&count);
	return g_strdup_printf("%s, %u not working", count, broken);
}

static HtmxNode *
bell_icon(void)
{
	HtmxNode *svg = htmx_node_new("svg");
	HtmxNode *body = htmx_node_new("path");
	HtmxNode *clapper = htmx_node_new("path");

	htmx_element_add_class(HTMX_ELEMENT(svg), "attn-bell-icon");
	htmx_element_set_attribute(HTMX_ELEMENT(svg), "viewBox", "0 0 24 24");
	htmx_element_set_attribute(HTMX_ELEMENT(svg), "aria-hidden", "true");
	htmx_element_set_attribute(HTMX_ELEMENT(svg), "focusable", "false");
	htmx_element_set_attribute(HTMX_ELEMENT(body), "d", "M12 3a6 6 0 0 0-6 6v4.2L4.2 16.6a.9.9 0 0 0 .8 1.4h14a.9.9 0 0 0 "
		".8-1.4L18 13.2V9a6 6 0 0 0-6-6z");
	htmx_element_set_attribute(HTMX_ELEMENT(clapper), "d", "M9.6 19.5a2.4 2.4 0 0 0 4.8 0z");
	add_owned(svg, body);
	add_owned(svg, clapper);
	return svg;
}

/* The bell for the user of @state; @on_page: the page that is open is the
 * one it links to. */
static HtmxNode *
bell_new(
	AxisWebAttention    *self,
	const AxisAuthState *state,
	gboolean             on_page
){
	g_autoptr(GPtrArray) shown = g_ptr_array_new();
	HtmxA *bell = htmx_a_new_with_href(PAGE_PATH);
	g_autofree gchar *label = NULL;
	guint broken = 0;
	guint i;

	items_for(self, state, shown, NULL);
	for (i = 0; i < shown->len; i++)
		broken += ((const AxisAttentionItem *)g_ptr_array_index(shown, i))->severity == AXIS_ATTENTION_BROKEN;
	label = bell_label(shown->len, broken);

	htmx_element_set_id(HTMX_ELEMENT(bell), BELL_ID);
	htmx_element_add_class(HTMX_ELEMENT(bell), "attn-bell");
	htmx_element_add_class(HTMX_ELEMENT(bell), shown->len == 0 ? "attn-bell-none" : broken > 0 ? "attn-bell-broken" :
		"attn-bell-waiting");
	htmx_element_set_attribute(HTMX_ELEMENT(bell), "aria-label", label);
	htmx_element_set_attribute(HTMX_ELEMENT(bell), "title", label);
	if (on_page)
		htmx_element_set_attribute(HTMX_ELEMENT(bell), "aria-current", "page");
	/* Told that the board changed, a page asks for its own bell: what it
	 * shows depends on who looks.  Only while the board is read. */
	if (is_live(self) && axis_web_get_hub(self->web) != NULL) {
		htmx_element_set_attribute(HTMX_ELEMENT(bell), "hx-get", BELL_PATH);
		htmx_element_set_attribute(HTMX_ELEMENT(bell), "hx-trigger", "sse:" AXIS_ATTENTION_EVENT);
		htmx_element_set_attribute(HTMX_ELEMENT(bell), "hx-swap", "outerHTML");
	}
	add_owned(bell, bell_icon());
	if (shown->len > 0) {
		HtmxSpan *badge = htmx_span_new();
		g_autofree gchar *figure = shown->len > 99 ? g_strdup("99+") : g_strdup_printf("%u", shown->len);

		htmx_element_add_class(HTMX_ELEMENT(badge), "attn-bell-badge");
		htmx_element_set_attribute(HTMX_ELEMENT(badge), "aria-hidden", "true");
		htmx_node_set_text_content(HTMX_NODE(badge), figure);
		add_owned(bell, badge);
	}
	return HTMX_NODE(bell);
}

HtmxNode *
axis_web_attention_bell_new(
	AxisWeb     *web,
	HtmxRequest *request,
	const gchar *request_path
){
	const AxisAuthState *state = axis_web_auth_state(request);
	AxisWebAttention *self = web != NULL ? axis_web_get_attention(web) : NULL;

	/* A user who can have no notice is shown no bell. */
	if (self == NULL || !may_have_notice(state))
		return NULL;
	if (is_live(self) && axis_web_get_hub(web) != NULL)
		axis_web_page_add_topic(request, AXIS_ATTENTION_TOPIC);
	return bell_new(self, state, g_strcmp0(request_path, PAGE_PATH) == 0);
}

static HtmxResponse *
fragment_response(
	HtmxNode *node,
	guint     status
){
	HtmxResponse *response = htmx_response_new();

	if (node != NULL) {
		g_autofree gchar *markup = htmx_element_render(HTMX_ELEMENT(node));

		htmx_response_set_content(response, markup);
	} else {
		htmx_response_set_content(response, "");
	}
	htmx_response_set_content_type(response, "text/html; charset=utf-8");
	if (status != 200)
		htmx_response_set_status(response, status);
	return response;
}

/* GET /attention/bell: the bell alone, from memory. */
static HtmxResponse *
handle_bell(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	AxisWebAttention *self = user_data;
	const AxisAuthState *state = axis_web_auth_state(request);
	HtmxRequestHeaders *headers = htmx_request_get_headers(request);
	const gchar *current = headers != NULL ? htmx_request_headers_get_current_url(headers) : NULL;
	g_autoptr(HtmxNode) bell = NULL;
	gboolean on_page = FALSE;

	/* Refused with nothing in it, which is also what takes the bell off a
	 * page left open by someone who may no longer have one. */
	if (!may_have_notice(state))
		return fragment_response(NULL, 403);

	if (current != NULL) {
		g_autoptr(GUri) uri = g_uri_parse(current, G_URI_FLAGS_NONE, NULL);

		on_page = uri != NULL && g_strcmp0(g_uri_get_path(uri), PAGE_PATH) == 0;
	}
	bell = bell_new(self, state, on_page);
	return fragment_response(bell, 200);
}

/* ── The page ───────────────────────────────────────────────────── */

static const struct
{
	AxisAttentionLevel  level;
	const gchar        *id;
	const gchar        *name;
	const gchar        *what;
} levels[] = {
	{ AXIS_ATTENTION_BROKEN, "attention-broken", "Not working",
	  "Something is broken, and media cannot arrive or be stored until it is mended." },
	{ AXIS_ATTENTION_WAITING, "attention-waiting", "Waiting for you",
	  "Axis stopped at a point where only a person can decide." },
	{ AXIS_ATTENTION_LOOK, "attention-look", "Worth a look", "Something is degraded, and the rest still works." }
};

static HtmxNode *
text_line(
	const gchar *css,
	const gchar *text
){
	HtmxP *line = htmx_p_new();

	htmx_element_add_class(HTMX_ELEMENT(line), css);
	htmx_node_set_text_content(HTMX_NODE(line), text);
	return HTMX_NODE(line);
}

/* A row: the sentence, since when, the page where it is dealt with, and the
 * one button.  @dismissed: a row of the list at the foot ("Show again"). */
static HtmxNode *
row_new(
	HtmxRequest             *request,
	const AxisAttentionItem *item,
	gboolean                 dismissed
){
	HtmxLi *row = htmx_li_new();
	HtmxP *sentence = htmx_p_new();
	HtmxP *meta = htmx_p_new();
	HtmxSpan *text = htmx_span_new();
	HtmxA *link = htmx_a_new_with_href(item->href);
	HtmxNode *form = axis_web_auth_post_form(request, dismissed ? RESTORE_PATH : DISMISS_PATH);
	g_autofree gchar *severity = g_strdup_printf("%d", (gint)item->severity);
	g_autofree gchar *count = g_strdup_printf("%" G_GUINT64_FORMAT, item->count);
	g_autofree gchar *newest = g_strdup_printf("%" G_GINT64_FORMAT, item->newest);

	htmx_element_add_class(HTMX_ELEMENT(row), "attn-row");
	htmx_element_add_class(HTMX_ELEMENT(sentence), "attn-text");
	htmx_node_set_text_content(HTMX_NODE(text), item->text);
	add_owned(sentence, text);
	if (item->name != NULL) {
		HtmxBdi *name = htmx_bdi_new();

		htmx_node_set_text_content(HTMX_NODE(name), item->name);
		add_owned(sentence, name);
	}
	if (item->tail != NULL) {
		HtmxSpan *tail = htmx_span_new();

		htmx_node_set_text_content(HTMX_NODE(tail), item->tail);
		add_owned(sentence, tail);
	}
	add_owned(row, sentence);

	htmx_element_add_class(HTMX_ELEMENT(meta), "attn-meta");
	if (item->since_us > 0) {
		HtmxSpan *since = htmx_span_new();
		g_autofree gchar *when = axis_web_format_time(item->since_us, "");
		g_autofree gchar *words = g_strdup_printf("Since %s", when);

		htmx_element_add_class(HTMX_ELEMENT(since), "attn-since");
		htmx_node_set_text_content(HTMX_NODE(since), words);
		add_owned(meta, since);
	}
	htmx_node_set_text_content(HTMX_NODE(link), item->page);
	add_owned(meta, link);
	add_owned(row, meta);

	htmx_element_add_class(HTMX_ELEMENT(form), "inline-form");
	htmx_element_add_class(HTMX_ELEMENT(form), "attn-form");
	add_owned(form, axis_web_auth_hidden("kind", item->kind));
	add_owned(form, axis_web_auth_hidden("subject", item->subject));
	if (!dismissed) {
		/* What is dismissed is what the page showed, not what came since. */
		add_owned(form, axis_web_auth_hidden("severity", severity));
		add_owned(form, axis_web_auth_hidden("count", count));
		add_owned(form, axis_web_auth_hidden("newest", newest));
	}
	add_owned(form, axis_web_auth_button(dismissed ? "Show again" : "Dismiss", "button-quiet"));
	add_owned(row, form);
	return HTMX_NODE(row);
}

static HtmxNode *
rows_new(
	HtmxRequest *request,
	GPtrArray   *items,
	gint         level,
	gboolean     dismissed
){
	HtmxUl *list = htmx_ul_new();
	guint i;

	htmx_element_add_class(HTMX_ELEMENT(list), "attn-rows");
	for (i = 0; i < items->len; i++) {
		const AxisAttentionItem *item = g_ptr_array_index(items, i);

		if (level == 0 || (gint)item->severity == level)
			add_owned(list, row_new(request, item, dismissed));
	}
	return HTMX_NODE(list);
}

/* The list: what the page asks for again when it is told the board changed. */
static HtmxNode *
list_new(
	AxisWebAttention *self,
	HtmxRequest      *request
){
	const AxisAuthState *state = axis_web_auth_state(request);
	g_autoptr(GPtrArray) shown = g_ptr_array_new();
	g_autoptr(GPtrArray) dismissed = g_ptr_array_new();
	HtmxDiv *list = htmx_div_new();
	guint i;
	guint k;

	htmx_element_set_id(HTMX_ELEMENT(list), LIST_ID);
	htmx_element_add_class(HTMX_ELEMENT(list), "attn-list");
	if (is_live(self) && axis_web_get_hub(self->web) != NULL) {
		htmx_element_set_attribute(HTMX_ELEMENT(list), "hx-get", PAGE_PATH "?part=list");
		htmx_element_set_attribute(HTMX_ELEMENT(list), "hx-trigger", "sse:" AXIS_ATTENTION_EVENT);
		htmx_element_set_attribute(HTMX_ELEMENT(list), "hx-swap", "outerHTML");
	}

	items_for(self, state, shown, dismissed);
	if (is_live(self) && self->readings == 0)
		add_owned(list, text_line("help", "Axis has not looked yet since it started. This page fills in within a "
			"minute."));
	else if (shown->len == 0)
		add_owned(list, text_line("all-clear", "Nothing needs attention."));

	for (i = 0; i < G_N_ELEMENTS(levels); i++) {
		HtmxSection *group;
		HtmxHeading *heading;
		guint n = 0;

		for (k = 0; k < shown->len; k++)
			n += ((const AxisAttentionItem *)g_ptr_array_index(shown, k))->severity == levels[i].level;
		if (n == 0)
			continue;
		group = htmx_section_new();
		heading = htmx_h2_new();
		htmx_element_add_class(HTMX_ELEMENT(group), "card");
		htmx_element_add_class(HTMX_ELEMENT(group), "attn-group");
		htmx_element_set_id(HTMX_ELEMENT(group), levels[i].id);
		htmx_node_set_text_content(HTMX_NODE(heading), levels[i].name);
		add_owned(group, heading);
		add_owned(group, text_line("help", levels[i].what));
		add_owned(group, rows_new(request, shown, levels[i].level, FALSE));
		add_owned(list, group);
	}

	/* A source that could not be read keeps what it last gave. */
	for (i = 0; i < self->sources->len; i++) {
		const Source *source = g_ptr_array_index(self->sources, i);
		g_autofree gchar *when = NULL;
		g_autofree gchar *words = NULL;

		if (!source_is_live(source) || !source->failed)
			continue;
		when = axis_web_format_time(source->read_us, NULL);
		words = when != NULL && when[0] != '\0' ?
			g_strdup_printf("Axis could not read %s at its last look. What this page says about them is unchanged "
				"since %s.", source->name, when) :
			g_strdup_printf("Axis could not read %s yet, so this page says nothing about them.", source->name);
		add_owned(list, text_line("attn-unread", words));
	}

	if (dismissed->len > 0) {
		HtmxNode *details = htmx_node_new("details");
		HtmxNode *summary = htmx_node_new("summary");
		g_autofree gchar *label = g_strdup_printf("Dismissed (%u)", dismissed->len);

		htmx_element_add_class(HTMX_ELEMENT(details), "attn-dismissed");
		htmx_element_set_id(HTMX_ELEMENT(details), "attention-dismissed");
		htmx_node_set_text_content(summary, label);
		add_owned(details, summary);
		add_owned(details, text_line("help", "You dismissed these. Each is still true, and comes back to the list above "
			"when it happens again, when there are more, or when it gets more serious."));
		add_owned(details, rows_new(request, dismissed, 0, TRUE));
		add_owned(list, details);
	}
	return HTMX_NODE(list);
}

/* ── The dashboard's card ───────────────────────────────────────── */

static const gchar *
level_name(AxisAttentionLevel level)
{
	guint i;

	for (i = 0; i < G_N_ELEMENTS(levels); i++) {
		if (levels[i].level == level)
			return levels[i].name;
	}
	return "";
}

/*
 * The dashboard's card "Needs attention" (slice N2): the head of the list the
 * page /attention shows this user, the most serious first, at most
 * %AXIS_ATTENTION_CARD_ROWS of them, with a link to the rest.  It is the
 * board's and nothing else's: the same waits, the same levels, the same
 * dismissals, so a figure never differs between the card, the bell and the
 * page.  No statement is run.  %NULL for a user who can have no notice.
 */
HtmxNode *
axis_web_attention_card_new(
	AxisWeb     *web,
	HtmxRequest *request
){
	AxisWebAttention *self = web != NULL ? axis_web_get_attention(web) : NULL;
	const AxisAuthState *state = axis_web_auth_state(request);
	g_autoptr(GPtrArray) shown = g_ptr_array_new();
	HtmxSection *card;
	HtmxDiv *head;
	HtmxHeading *heading;
	HtmxA *title;
	gint64 read_us = 0;
	guint i;

	if (self == NULL || !may_have_notice(state))
		return NULL;
	items_for(self, state, shown, NULL);

	card = htmx_section_new();
	head = htmx_div_new();
	heading = htmx_h2_new();
	title = htmx_a_new_with_href(PAGE_PATH);
	htmx_element_add_class(HTMX_ELEMENT(card), "card");
	htmx_element_add_class(HTMX_ELEMENT(card), "dash-card");
	htmx_element_set_id(HTMX_ELEMENT(card), "dash-attention");
	htmx_element_add_class(HTMX_ELEMENT(head), "card-head");
	htmx_node_set_text_content(HTMX_NODE(title), "Needs attention");
	add_owned(heading, title);
	add_owned(head, heading);
	add_owned(card, head);

	if (shown->len == 0) {
		add_owned(card, text_line("all-clear", "Nothing needs attention."));
	} else {
		HtmxUl *list = htmx_ul_new();

		htmx_element_add_class(HTMX_ELEMENT(list), "attention-list");
		for (i = 0; i < shown->len && i < AXIS_ATTENTION_CARD_ROWS; i++) {
			const AxisAttentionItem *item = g_ptr_array_index(shown, i);
			HtmxLi *entry = htmx_li_new();
			HtmxSpan *level = htmx_span_new();
			HtmxA *link = htmx_a_new_with_href(item->href);
			HtmxSpan *text = htmx_span_new();

			/* The level in words: colour is never the only sign. */
			htmx_element_add_class(HTMX_ELEMENT(level), "badge");
			htmx_element_add_class(HTMX_ELEMENT(level), item->severity == AXIS_ATTENTION_BROKEN ? "badge-failed" :
				item->severity == AXIS_ATTENTION_WAITING ? "badge-warn" : "badge-note");
			htmx_element_add_class(HTMX_ELEMENT(level), "attention-level");
			htmx_node_set_text_content(HTMX_NODE(level), level_name(item->severity));
			htmx_node_set_text_content(HTMX_NODE(text), item->text);
			add_owned(link, text);
			if (item->name != NULL) {
				HtmxBdi *name = htmx_bdi_new();

				htmx_node_set_text_content(HTMX_NODE(name), item->name);
				add_owned(link, name);
			}
			if (item->tail != NULL) {
				HtmxSpan *tail = htmx_span_new();

				htmx_node_set_text_content(HTMX_NODE(tail), item->tail);
				add_owned(link, tail);
			}
			add_owned(entry, level);
			add_owned(entry, link);
			add_owned(list, entry);
		}
		add_owned(card, list);
	}
	if (shown->len > 0) {
		HtmxP *foot = htmx_p_new();
		HtmxA *link = htmx_a_new_with_href(PAGE_PATH);
		g_autofree gchar *words = shown->len > AXIS_ATTENTION_CARD_ROWS ?
			g_strdup_printf("All %u on the Needs attention page", shown->len) :
			g_strdup("Open the Needs attention page");

		htmx_element_add_class(HTMX_ELEMENT(foot), "card-foot");
		htmx_node_set_text_content(HTMX_NODE(link), words);
		add_owned(foot, link);
		add_owned(card, foot);
	}
	for (i = 0; i < self->sources->len; i++)
		read_us = MAX(read_us, ((const Source *)g_ptr_array_index(self->sources, i))->read_us);
	add_owned(card, axis_web_dashboard_as_of("As of", read_us));
	return HTMX_NODE(card);
}

/* GET /attention, and /attention?part=list for the list alone. */
static HtmxResponse *
handle_page(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	AxisWebAttention *self = user_data;
	g_autoptr(HtmxDiv) content = NULL;
	HtmxHeading *title;

	if (g_strcmp0(htmx_request_get_query_param(request, "part"), "list") == 0 && htmx_request_is_htmx(request)) {
		g_autoptr(HtmxNode) list = list_new(self, request);

		return fragment_response(list, 200);
	}

	content = htmx_div_new();
	title = htmx_h1_new();
	htmx_element_add_class(HTMX_ELEMENT(content), "attn-page");
	htmx_node_set_text_content(HTMX_NODE(title), "Needs attention");
	add_owned(content, title);
	add_owned(content, axis_web_page_intro("What stops media from arriving or being stored and waits for a person. Each "
		"line says what waits and links to the page where you deal with it. A line goes away by itself once its cause "
		"is dealt with; Dismiss only takes it out of your own count."));
	add_owned(content, list_new(self, request));
	return axis_web_page_respond(request, "Needs attention", HTMX_NODE(content));
}

/* ── Dismiss and Show again ─────────────────────────────────────── */

typedef struct
{
	AxisWebAttention *owner;
	AxisDbInterrupt  *interrupt;	/* borrowed: the board waits for the thread */
	HtmxRequest      *request;
	HtmxResponse     *deferred;
	gboolean          restore;
	Dismissal         what;
	GError           *error;
} Post;

static void
post_free(Post *post)
{
	g_free(post->what.kind);
	g_free(post->what.subject);
	g_clear_error(&post->error);
	g_clear_object(&post->request);
	g_clear_object(&post->deferred);
	g_free(post);
}

/* The dismissals in memory follow, and the user's other pages are told. */
static void
post_apply(Post *post)
{
	AxisWebAttention *self = post->owner;
	gchar *key = dismissal_key(post->what.user_id, post->what.kind, post->what.subject);
	guint i;

	/* The rows are still being read for the first time: what the person
	 * just did is the newer, whatever that reading brings for this key. */
	if (self->pool != NULL && !self->dismissals_loaded)
		g_hash_table_add(self->touched, g_strdup(key));
	/* A row of this key waiting to be deleted (its notice had ended) is not
	 * this one's. */
	for (i = self->ended->len; i > 0; i--) {
		const Dismissal *ended = g_ptr_array_index(self->ended, i - 1);

		if (ended->user_id == post->what.user_id && strcmp(ended->kind, post->what.kind) == 0 &&
		    strcmp(ended->subject, post->what.subject) == 0)
			g_ptr_array_remove_index(self->ended, i - 1);
	}
	if (post->restore) {
		g_hash_table_remove(self->dismissals, key);
		g_free(key);
	} else {
		g_hash_table_replace(self->dismissals, key, dismissal_copy(&post->what));
	}
	tell(self);
}

static void
post_thread(
	GTask        *gtask,
	gpointer      source_object,
	gpointer      task_data,
	GCancellable *cancellable
){
	Post *post = task_data;
	g_autofree gchar *user = g_strdup_printf("%" G_GINT64_FORMAT, post->what.user_id);
	g_autofree gchar *severity = g_strdup_printf("%d", post->what.severity);
	g_autofree gchar *count = g_strdup_printf("%" G_GUINT64_FORMAT, post->what.count);
	g_autofree gchar *newest = g_strdup_printf("%" G_GINT64_FORMAT, post->what.newest);
	const gchar *params[6];

	params[0] = user;
	params[1] = post->what.kind;
	params[2] = post->what.subject;
	params[3] = severity;
	params[4] = count;
	params[5] = newest;
	/* A stop calls the statement off, and the post is answered with that. */
	axis_db_interrupt_attach(post->interrupt);
	if (post->restore)
		axis_db_pool_exec(post->owner->pool, "DELETE FROM attention_dismissal WHERE user_id = $1::bigint"
			" AND kind = $2::text AND subject = $3::text", 3, params, NULL, &post->error);
	else
		axis_db_pool_exec(post->owner->pool, "INSERT INTO attention_dismissal (user_id, kind, subject, severity,"
			" count, newest) VALUES ($1::bigint, $2::text, $3::text, $4::smallint, $5::bigint, $6::bigint)"
			" ON CONFLICT (user_id, kind, subject) DO UPDATE SET severity = excluded.severity,"
			" count = excluded.count, newest = excluded.newest, dismissed_at = now()", 6, params, NULL, &post->error);
	axis_db_interrupt_attach(NULL);
	g_task_return_boolean(gtask, TRUE);
}

static void
post_done(
	GObject      *source_object,
	GAsyncResult *result,
	gpointer      user_data
){
	Post *post = user_data;
	HtmxResponse *response;

	post->owner->pending--;
	if (post->error != NULL) {
		response = axis_web_internal_error(post->request, post->error);
	} else {
		post_apply(post);
		response = axis_web_auth_redirect(post->request, PAGE_PATH);
	}
	htmx_response_complete(post->deferred, response);
	g_object_unref(response);
	post_free(post);
}

static gboolean
form_number(
	HtmxRequest *request,
	const gchar *name,
	gint64      *value
){
	const gchar *text = htmx_request_get_form_value(request, name);

	return text != NULL && g_ascii_string_to_signed(text, 10, 0, G_MAXINT64, value, NULL);
}

static HtmxResponse *
bad_form(HtmxRequest *request)
{
	return axis_web_error_respond(request, 400, "Not possible",
		"This is not a form of the Needs attention page. Go back to that page and use the button there.");
}

/* The item of the board the user of @state may see that is @kind's and
 * @subject's, or %NULL. */
static const AxisAttentionItem *
find_item(
	AxisWebAttention    *self,
	const AxisAuthState *state,
	const gchar         *kind,
	const gchar         *subject
){
	guint i;
	guint k;

	for (i = 0; i < self->sources->len; i++) {
		const Source *source = g_ptr_array_index(self->sources, i);

		if (!source_is_live(source))
			continue;
		for (k = 0; k < source->items->len; k++) {
			const AxisAttentionItem *item = g_ptr_array_index(source->items, k);

			if (may_see(state, item) && strcmp(item->kind, kind) == 0 && strcmp(item->subject, subject) == 0)
				return item;
		}
	}
	return NULL;
}

/*
 * POST /attention/dismiss and /attention/restore (the shell has checked the
 * session, the role and the CSRF token).  A dismissal is of a notice the
 * user is shown now, and never of more than is there now: the level, the
 * number and the "newest" stored are the lower of what the page showed and
 * what the board holds.
 */
static HtmxResponse *
handle_post(
	HtmxRequest *request,
	gboolean     restore,
	gpointer     user_data
){
	AxisWebAttention *self = user_data;
	const AxisAuthState *state = axis_web_auth_state(request);
	const gchar *kind = htmx_request_get_form_value(request, "kind");
	const gchar *subject = htmx_request_get_form_value(request, "subject");
	HtmxResponse *returned;
	GTask *gtask;
	Post *post;

	if (!may_have_notice(state))
		return axis_web_error_respond(request, 403, "Not allowed", "Only an administrator can do this.");
	if (subject == NULL)
		subject = "";
	if (!kind_is_valid(kind) || !subject_is_valid(subject))
		return bad_form(request);

	post = g_new0(Post, 1);
	post->owner = self;
	post->interrupt = self->interrupt;
	post->restore = restore;
	post->what.user_id = state->user_id;
	post->what.kind = g_strdup(kind);
	post->what.subject = g_strdup(subject);
	if (!restore) {
		const AxisAttentionItem *item;
		gint64 severity = 0;
		gint64 count = 0;
		gint64 newest = 0;

		if (!form_number(request, "severity", &severity) || !level_is_valid(severity) ||
		    !form_number(request, "count", &count) || !form_number(request, "newest", &newest)) {
			post_free(post);
			return bad_form(request);
		}
		item = find_item(self, state, kind, subject);
		/* It ended meanwhile: there is nothing to dismiss. */
		if (item == NULL) {
			post_free(post);
			return axis_web_auth_redirect(request, PAGE_PATH);
		}
		post->what.severity = (gint)MIN(severity, (gint64)item->severity);
		post->what.count = MIN((guint64)count, item->count);
		post->what.newest = MIN(newest, item->newest);
	}

	/* Without a database (a test) the dismissals are in memory only. */
	if (self->pool == NULL) {
		post_apply(post);
		post_free(post);
		return axis_web_auth_redirect(request, PAGE_PATH);
	}
	if (self->stopping || self->pending >= MAX_PENDING) {
		HtmxResponse *busy = axis_web_error_respond(request, 503, "Axis is busy",
			"Axis is busy. Try again in a few seconds.");

		htmx_response_add_header(busy, "Retry-After", "5");
		post_free(post);
		return busy;
	}

	post->request = g_object_ref(request);
	post->deferred = htmx_response_new_deferred();
	returned = g_object_ref(post->deferred);
	self->pending++;
	gtask = g_task_new(NULL, NULL, post_done, post);
	g_task_set_task_data(gtask, post, NULL);
	g_task_run_in_thread(gtask, post_thread);
	g_object_unref(gtask);
	return returned;
}

static HtmxResponse *
handle_dismiss(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	return handle_post(request, FALSE, user_data);
}

static HtmxResponse *
handle_restore(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	return handle_post(request, TRUE, user_data);
}

void
axis_web_attention_register(
	AxisWebAttention *self,
	AxisWeb          *web
){
	/* Every notice of this slice is an administrator's, so the page and the
	 * posts are too.  The bell's own request is checked by its handler: its
	 * refusal has to be an empty one (see handle_bell()). */
	axis_web_route_full(web, HTMX_METHOD_GET, PAGE_PATH, AXIS_WEB_ROUTE_ADMIN, handle_page, self);
	axis_web_route_full(web, HTMX_METHOD_GET, BELL_PATH, AXIS_WEB_ROUTE_DEFAULT, handle_bell, self);
	axis_web_route_full(web, HTMX_METHOD_POST, DISMISS_PATH, AXIS_WEB_ROUTE_ADMIN, handle_dismiss, self);
	axis_web_route_full(web, HTMX_METHOD_POST, RESTORE_PATH, AXIS_WEB_ROUTE_ADMIN, handle_restore, self);
}
