/*
 * axis-web-dashboard.c - the dashboard: the page a user lands on
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The shell owns the page and its cards; the modules fill them.  Each
 * module adds a part (axis_web_add_dashboard_part()): a function that reads
 * its figures on a worker thread and one that turns them into pieces of
 * cards on the main loop.
 *
 * Once the board is started (axis_web_dashboard_start(); the daemon does),
 * no page view reads anything: the figures every part last gave are kept in
 * memory, and a view builds the page from them at once.  They are read again
 * in the background, all the parts on one worker thread one after another,
 * as the board of what needs attention is (axis-web-attention.c, whose
 * pattern this is): when something happened (the events that board is told
 * of: a job ended, an import, a person's action), when a dashboard is
 * opened, and every %AXIS_WEB_DASHBOARD_PERIOD_S; never sooner than
 * %AXIS_WEB_DASHBOARD_MIN_GAP_S after the reading before.  After a reading
 * the open dashboards are told (%AXIS_WEB_DASHBOARD_EVENT on
 * %AXIS_WEB_DASHBOARD_TOPIC, which carries nothing) and each asks for its
 * cards again ("/?part=cards", from memory too).  A part that has not been
 * read yet shows "Counting..." in its card; one whose reading failed shows
 * what it last gave.  A stop calls a reading off through its AxisDbInterrupt
 * and never waits on the database.
 *
 * A shell that was never started (the tests' pages, mostly) has no reader:
 * there a request runs every part of a module that is switched on, all
 * their reading on one worker thread one after another, then builds the
 * page from what they gave.
 *
 * The cards:
 *
 *   Coming in   what is downloading, imported, wanted (no module yet: the
 *               card is absent until one adds to it)
 *   Space       each volume, how full it is, and the collection
 *   Now         what is running
 *   Attention   the head of the list of the page "Needs attention", drawn
 *               from the board that page and the bell are drawn from
 *               (axis-web-attention.c), for a user who can have a notice
 *   Savings     what converting would save
 *
 * Every piece says when its figures were taken and links to the page that
 * shows the same figures.  The daemon's version is in the footer of every
 * page, and its modules are on the Modules page.
 */

#include "web/axis-web-private.h"

#include <string.h>

#include "web/axis-web-hub.h"

/* Dashboards waiting for their thread beyond this are answered 503. */
#define MAX_PENDING 8

#define CARDS_ID   "dashboard-cards"
#define CARDS_PATH "/?part=cards"

typedef struct
{
	AxisModuleSwitch            *sw;	/* its module's flag, or %NULL */
	gboolean                     admins_only;	/* nothing of it for anyone else */
	AxisWebDashboardCollectFunc  collect;
	AxisWebDashboardRenderFunc   render;
	GDestroyNotify               free_figures;
	gpointer                     user_data;
	gint                         card;	/* the card it says "Counting..." in; -1: none */
	/* The board's: what it last gave, and whether it ever gave anything. */
	gpointer                     figures;
	gboolean                     known;
} Part;

struct _AxisWebDashboardParts
{
	AxisWeb   *web;
	GPtrArray *parts;	/* of Part* */
	guint      pending;
	gboolean   stopping;
	/* The board (started only). */
	AxisDbInterrupt *interrupt;	/* raised by a stop: ends the thread's waits on the database */
	gboolean   started;
	gboolean   reading;	/* a reading is on its thread */
	gboolean   stale;
	guint      soon_id;
	guint      period_id;
	gint64     last_read_us;	/* monotonic; 0: never */
	guint      min_gap_ms;
	guint      period_ms;
	guint      readings;
};

typedef struct
{
	gint      position;
	gchar    *href;
	HtmxNode *piece;
} Piece;

struct _AxisWebDashboard
{
	GPtrArray *pieces[AXIS_WEB_DASHBOARD_SLOTS];	/* of Piece* */
};

/* The cards' headings, in the order of the slots. */
static const gchar *const slot_titles[AXIS_WEB_DASHBOARD_SLOTS] = {
	"Coming into the library", "Disk space", "Running now", "Space savings",
	"Still needed before Axis can download"
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

static void
part_forget(Part *part)
{
	if (part->figures != NULL && part->free_figures != NULL)
		part->free_figures(part->figures);
	part->figures = NULL;
	part->known = FALSE;
}

static void
part_free(Part *part)
{
	part_forget(part);
	axis_module_switch_unref(part->sw);
	g_free(part);
}

static gboolean
part_is_live(const Part *part)
{
	return part->sw == NULL || axis_module_switch_is_on(part->sw);
}

AxisWebDashboardParts *
axis_web_dashboard_parts_new(AxisWeb *web)
{
	AxisWebDashboardParts *self = g_new0(AxisWebDashboardParts, 1);

	self->web = web;
	self->parts = g_ptr_array_new_with_free_func((GDestroyNotify)part_free);
	self->interrupt = axis_db_interrupt_new();
	self->min_gap_ms = AXIS_WEB_DASHBOARD_MIN_GAP_S * 1000;
	self->period_ms = AXIS_WEB_DASHBOARD_PERIOD_S * 1000;
	return self;
}

void
axis_web_dashboard_parts_stop(AxisWebDashboardParts *self)
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
	/* As the board of what needs attention stops (axis_web_attention_stop()):
	 * the reading on its thread runs the modules' parts and is waited for
	 * before they are stopped, and that wait may not depend on the database
	 * answering, so the thread's work is called off first: the wait on the
	 * database it is in ends at once, what it still tries fails without
	 * waiting, and no further part is read.  What such a reading gave is
	 * thrown away. */
	axis_db_interrupt_raise(self->interrupt);
	while (self->pending > 0 || self->reading)
		g_main_context_iteration(NULL, TRUE);
}

void
axis_web_dashboard_parts_free(AxisWebDashboardParts *self)
{
	if (self == NULL)
		return;

	axis_web_dashboard_parts_stop(self);
	/* No thread has it attached: the stop waited for the reading. */
	axis_db_interrupt_free(self->interrupt);
	g_ptr_array_free(self->parts, TRUE);
	g_free(self);
}

void
axis_web_dashboard_parts_add(
	AxisWebDashboardParts       *self,
	AxisModuleSwitch            *sw,
	gboolean                     admins_only,
	AxisWebDashboardCollectFunc  collect,
	AxisWebDashboardRenderFunc   render,
	GDestroyNotify               free_figures,
	gpointer                     user_data
){
	Part *part = g_new0(Part, 1);

	part->sw = sw;
	part->admins_only = admins_only;
	part->collect = collect;
	part->render = render;
	part->free_figures = free_figures;
	part->user_data = user_data;
	part->card = -1;
	g_ptr_array_add(self->parts, part);
}

void
axis_web_dashboard_parts_set_card(
	AxisWebDashboardParts *self,
	AxisWebDashboardSlot   slot
){
	g_return_if_fail(self->parts->len > 0 && (guint)slot < AXIS_WEB_DASHBOARD_SLOTS);

	((Part *)g_ptr_array_index(self->parts, self->parts->len - 1))->card = (gint)slot;
}

/* ── The board ──────────────────────────────────────────────────── */

static void
piece_free(Piece *piece)
{
	g_free(piece->href);
	g_clear_object(&piece->piece);
	g_free(piece);
}

static AxisWebDashboard *
board_new(void)
{
	AxisWebDashboard *board = g_new0(AxisWebDashboard, 1);
	guint i;

	for (i = 0; i < AXIS_WEB_DASHBOARD_SLOTS; i++)
		board->pieces[i] = g_ptr_array_new_with_free_func((GDestroyNotify)piece_free);
	return board;
}

static void
board_free(AxisWebDashboard *board)
{
	guint i;

	for (i = 0; i < AXIS_WEB_DASHBOARD_SLOTS; i++)
		g_ptr_array_unref(board->pieces[i]);
	g_free(board);
}

void
axis_web_dashboard_add(
	AxisWebDashboard     *board,
	AxisWebDashboardSlot  slot,
	gint                  position,
	const gchar          *href,
	HtmxNode             *piece
){
	Piece *entry;

	g_return_if_fail(board != NULL);
	g_return_if_fail(HTMX_IS_NODE(piece));
	if ((guint)slot >= AXIS_WEB_DASHBOARD_SLOTS) {
		g_critical("no dashboard card %d", slot);
		g_object_unref(piece);
		return;
	}

	entry = g_new0(Piece, 1);
	entry->position = position;
	entry->href = g_strdup(href);
	entry->piece = piece;
	g_ptr_array_add(board->pieces[slot], entry);
}

HtmxNode *
axis_web_dashboard_as_of(
	const gchar *prefix,
	gint64       us
){
	HtmxP *line = htmx_p_new();
	g_autofree gchar *when = axis_web_format_time(us, NULL);
	g_autofree gchar *text = when != NULL && when[0] != '\0' ?
		g_strdup_printf("%s %s.", prefix, when) : g_strdup("Not read yet.");

	htmx_element_add_class(HTMX_ELEMENT(line), "as-of");
	htmx_node_set_text_content(HTMX_NODE(line), text);
	return HTMX_NODE(line);
}

static gint
compare_pieces(
	gconstpointer a,
	gconstpointer b
){
	const Piece *x = *(Piece *const *)a;
	const Piece *y = *(Piece *const *)b;

	return x->position < y->position ? -1 : x->position > y->position;
}

/* A card with its heading, linking to the page behind it when there is one. */
static HtmxNode *
card_new(
	const gchar *title,
	const gchar *href,
	const gchar *id
){
	HtmxSection *card = htmx_section_new();
	HtmxDiv *head = htmx_div_new();
	HtmxHeading *heading = htmx_h2_new();

	htmx_element_add_class(HTMX_ELEMENT(card), "card");
	htmx_element_add_class(HTMX_ELEMENT(card), "dash-card");
	htmx_element_set_id(HTMX_ELEMENT(card), id);
	htmx_element_add_class(HTMX_ELEMENT(head), "card-head");
	if (href != NULL) {
		HtmxA *link = htmx_a_new_with_href(href);

		htmx_node_set_text_content(HTMX_NODE(link), title);
		add_owned(heading, link);
	} else {
		htmx_node_set_text_content(HTMX_NODE(heading), title);
	}
	add_owned(head, heading);
	add_owned(card, head);

	return HTMX_NODE(card);
}

static HtmxNode *
slot_card(
	AxisWebDashboard     *board,
	AxisWebDashboardSlot  slot
){
	static const gchar *const ids[AXIS_WEB_DASHBOARD_SLOTS] = {
		"dash-coming-in", "dash-space", "dash-now", "dash-savings", "dash-setup"
	};
	GPtrArray *pieces = board->pieces[slot];
	const gchar *href = NULL;
	HtmxNode *card;
	guint i;

	if (pieces->len == 0)
		return NULL;
	g_ptr_array_sort(pieces, compare_pieces);
	for (i = 0; i < pieces->len && href == NULL; i++)
		href = ((Piece *)g_ptr_array_index(pieces, i))->href;

	card = card_new(slot_titles[slot], href, ids[slot]);
	for (i = 0; i < pieces->len; i++) {
		Piece *piece = g_ptr_array_index(pieces, i);

		add_owned(card, g_steal_pointer(&piece->piece));
	}
	return card;
}

static gboolean
board_is_live(const AxisWebDashboardParts *self)
{
	return self->started && !self->stopping;
}

/* What a part that has not been read yet says in its card. */
static HtmxNode *
counting_new(void)
{
	HtmxP *line = htmx_p_new();

	htmx_element_add_class(HTMX_ELEMENT(line), "help");
	htmx_element_add_class(HTMX_ELEMENT(line), "dash-counting");
	htmx_node_set_text_content(HTMX_NODE(line), "Counting...");
	return HTMX_NODE(line);
}

/*
 * The cards.  @figures: one per part of @parts, as a request read them; or
 * %NULL for the board's: what each part last gave, "Counting..." for a part
 * that has given nothing yet, and nothing of an administrators' part for
 * anyone else.
 */
static HtmxNode *
build_cards(
	GPtrArray   *parts,
	gpointer    *figures,
	HtmxRequest *request
){
	AxisWebDashboard *board = board_new();
	HtmxDiv *grid = htmx_div_new();
	gboolean admin = figures == NULL && axis_web_request_is_admin(request);
	HtmxNode *card;
	guint i;

	for (i = 0; i < parts->len; i++) {
		Part *part = g_ptr_array_index(parts, i);

		/* A module switched off while its figures were read shows nothing. */
		if (!part_is_live(part))
			continue;
		if (figures != NULL) {
			part->render(board, request, figures[i], part->user_data);
		} else if (part->admins_only && !admin) {
			continue;
		} else if (part->collect != NULL && !part->known) {
			if (part->card >= 0)
				axis_web_dashboard_add(board, (AxisWebDashboardSlot)part->card, G_MAXINT, NULL, counting_new());
		} else {
			part->render(board, request, part->figures, part->user_data);
		}
	}

	htmx_element_set_id(HTMX_ELEMENT(grid), CARDS_ID);
	htmx_element_add_class(HTMX_ELEMENT(grid), "dashboard");

	/* What is still needed to download anything comes before everything,
	 * across the page, until it is all there. */
	card = slot_card(board, AXIS_WEB_DASHBOARD_SETUP);
	if (card != NULL) {
		htmx_element_add_class(HTMX_ELEMENT(card), "card-wide");
		add_owned(grid, card);
	}
	/* What is coming in leads, across the page, once there is any. */
	card = slot_card(board, AXIS_WEB_DASHBOARD_COMING_IN);
	if (card != NULL) {
		htmx_element_add_class(HTMX_ELEMENT(card), "card-wide");
		add_owned(grid, card);
	}
	card = slot_card(board, AXIS_WEB_DASHBOARD_SPACE);
	if (card != NULL)
		add_owned(grid, card);
	card = slot_card(board, AXIS_WEB_DASHBOARD_NOW);
	if (card != NULL)
		add_owned(grid, card);
	/* What needs attention is the board's (axis-web-attention.c): the head of
	 * the list the page /attention shows this user, from memory.  No part
	 * adds to it, and a user who can have no notice has no card. */
	card = axis_web_attention_card_new(axis_web_from_request(request), request);
	if (card != NULL)
		add_owned(grid, card);
	card = slot_card(board, AXIS_WEB_DASHBOARD_SAVINGS);
	if (card != NULL)
		add_owned(grid, card);

	board_free(board);
	return HTMX_NODE(grid);
}

/* What the cards are told apart by: a page that has these asks for nothing. */
static gchar *
cards_mark(HtmxNode *cards)
{
	g_autofree gchar *markup = htmx_element_render(HTMX_ELEMENT(cards));

	return g_compute_checksum_for_string(G_CHECKSUM_SHA256, markup, -1);
}

/* The cards of a started board ask for themselves again when they are told
 * of a reading, saying which they are. */
static void
cards_go_live(
	AxisWebDashboardParts *self,
	HtmxNode              *cards
){
	g_autofree gchar *mark = NULL;
	g_autofree gchar *path = NULL;

	if (!board_is_live(self) || axis_web_get_hub(self->web) == NULL)
		return;
	mark = cards_mark(cards);
	path = g_strdup_printf(CARDS_PATH "&have=%.16s", mark);
	htmx_element_set_attribute(HTMX_ELEMENT(cards), "hx-get", path);
	htmx_element_set_attribute(HTMX_ELEMENT(cards), "hx-trigger", "sse:" AXIS_WEB_DASHBOARD_EVENT);
	htmx_element_set_attribute(HTMX_ELEMENT(cards), "hx-swap", "outerHTML");
}

static HtmxResponse *
build_page(
	AxisWebDashboardParts *self,
	GPtrArray             *parts,
	gpointer              *figures,
	HtmxRequest           *request
){
	g_autoptr(HtmxDiv) content = htmx_div_new();
	HtmxHeading *title = htmx_h1_new();
	HtmxNode *cards = build_cards(parts, figures, request);

	htmx_node_set_text_content(HTMX_NODE(title), "Dashboard");
	add_owned(content, title);
	if (figures == NULL) {
		cards_go_live(self, cards);
		if (board_is_live(self) && axis_web_get_hub(self->web) != NULL)
			axis_web_page_add_topic(request, AXIS_WEB_DASHBOARD_TOPIC);
	}
	add_owned(content, cards);
	return axis_web_page_respond(request, "Dashboard", HTMX_NODE(content));
}

/* ── The board: the figures in memory, read in the background ────── */

typedef struct
{
	AxisWebDashboardParts *owner;
	AxisDbInterrupt       *interrupt;	/* borrowed: the stop waits for the thread */
	GPtrArray             *parts;	/* of Part*, borrowed: the owner keeps them */
	gpointer              *figures;	/* one per part */
} Reading;

static void
schedule(AxisWebDashboardParts *self);

/* On the worker thread: nothing of the board is touched. */
static void
reading_thread(
	GTask        *gtask,
	gpointer      source,
	gpointer      task_data,
	GCancellable *cancellable
){
	Reading *reading = task_data;
	guint i;

	axis_db_interrupt_attach(reading->interrupt);
	for (i = 0; i < reading->parts->len; i++) {
		Part *part = g_ptr_array_index(reading->parts, i);

		/* Called off: the parts after this one are not read. */
		if (axis_db_interrupt_is_raised(reading->interrupt))
			break;
		reading->figures[i] = part->collect(part->user_data);
	}
	/* The thread is the pool's again: what runs on it next is not ours. */
	axis_db_interrupt_attach(NULL);
	g_task_return_boolean(gtask, TRUE);
}

static void
reading_done(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	Reading *reading = user_data;
	AxisWebDashboardParts *self = reading->owner;
	AxisWebHub *hub;
	guint i;

	self->reading = FALSE;
	self->last_read_us = g_get_monotonic_time();
	for (i = 0; i < reading->parts->len; i++) {
		Part *part = g_ptr_array_index(reading->parts, i);

		/* A reading called off is thrown away, whole. */
		if (self->stopping) {
			if (reading->figures[i] != NULL && part->free_figures != NULL)
				part->free_figures(reading->figures[i]);
			continue;
		}
		/* A part that gave nothing (it has none, or it could not be read)
		 * keeps what it last gave, if it ever gave anything. */
		part->known = TRUE;
		if (reading->figures[i] == NULL)
			continue;
		if (part->figures != NULL && part->free_figures != NULL)
			part->free_figures(part->figures);
		part->figures = reading->figures[i];
	}
	g_ptr_array_unref(reading->parts);
	g_free(reading->figures);
	g_free(reading);
	if (self->stopping)
		return;
	/* A part that was not read (its module is off) has nothing. */
	for (i = 0; i < self->parts->len; i++) {
		Part *part = g_ptr_array_index(self->parts, i);

		if (!part_is_live(part))
			part_forget(part);
	}
	self->readings++;
	/* The event carries nothing of the figures: each page asks for its own. */
	hub = axis_web_get_hub(self->web);
	if (hub != NULL && axis_web_hub_count_subscribers(hub, AXIS_WEB_DASHBOARD_TOPIC) > 0)
		axis_web_hub_publish(hub, AXIS_WEB_DASHBOARD_TOPIC, AXIS_WEB_DASHBOARD_EVENT, "changed");
	schedule(self);
}

static void
begin_reading(AxisWebDashboardParts *self)
{
	Reading *reading = g_new0(Reading, 1);
	GTask *gtask;
	guint i;

	self->reading = TRUE;
	self->stale = FALSE;
	reading->owner = self;
	reading->interrupt = self->interrupt;
	reading->parts = g_ptr_array_new();
	for (i = 0; i < self->parts->len; i++) {
		Part *part = g_ptr_array_index(self->parts, i);

		/* An administrators' part is read for the board, and shown to them
		 * only. */
		if (part_is_live(part) && part->collect != NULL)
			g_ptr_array_add(reading->parts, part);
	}
	reading->figures = g_new0(gpointer, MAX(reading->parts->len, 1));
	gtask = g_task_new(NULL, NULL, reading_done, reading);
	g_task_set_task_data(gtask, reading, NULL);
	g_task_run_in_thread(gtask, reading_thread);
	g_object_unref(gtask);
}

static gboolean
on_soon(gpointer data)
{
	AxisWebDashboardParts *self = data;

	self->soon_id = 0;
	if (board_is_live(self) && !self->reading)
		begin_reading(self);
	return G_SOURCE_REMOVE;
}

/* Arms the next reading of stale figures: at once the first time, else no
 * sooner than the gap after the last one finished.  A burst of events is one
 * reading. */
static void
schedule(AxisWebDashboardParts *self)
{
	gint64 wait_ms = 0;

	if (!board_is_live(self) || !self->stale || self->reading || self->soon_id != 0)
		return;
	if (self->last_read_us != 0)
		wait_ms = (self->last_read_us + (gint64)self->min_gap_ms * 1000 - g_get_monotonic_time()) / 1000;
	self->soon_id = g_timeout_add((guint)CLAMP(wait_ms, 0, G_MAXINT), on_soon, self);
}

static gboolean
on_period(gpointer data)
{
	AxisWebDashboardParts *self = data;

	/* Some figures are about time passing, and an event can be missed. */
	self->stale = TRUE;
	schedule(self);
	return G_SOURCE_CONTINUE;
}

void
axis_web_dashboard_parts_changed(AxisWebDashboardParts *self)
{
	if (self == NULL || !board_is_live(self))
		return;
	self->stale = TRUE;
	schedule(self);
}

void
axis_web_dashboard_start(AxisWeb *web)
{
	AxisWebDashboardParts *self;

	g_return_if_fail(web != NULL);

	self = axis_web_get_dashboard_parts(web);
	if (self->started || self->stopping)
		return;
	self->started = TRUE;
	self->period_id = g_timeout_add(self->period_ms, on_period, self);
	self->stale = TRUE;
	schedule(self);
}

void
axis_web_dashboard_set_tuning(
	AxisWeb *web,
	guint    min_gap_ms,
	guint    period_ms
){
	AxisWebDashboardParts *self;

	g_return_if_fail(web != NULL);

	self = axis_web_get_dashboard_parts(web);
	if (min_gap_ms != 0)
		self->min_gap_ms = min_gap_ms;
	if (period_ms != 0 && period_ms != self->period_ms) {
		self->period_ms = period_ms;
		if (self->period_id != 0) {
			g_source_remove(self->period_id);
			self->period_id = g_timeout_add(self->period_ms, on_period, self);
		}
	}
}

guint
axis_web_dashboard_get_readings(AxisWeb *web)
{
	g_return_val_if_fail(web != NULL, 0);
	return axis_web_get_dashboard_parts(web)->readings;
}

/* ── The request ────────────────────────────────────────────────── */

typedef struct
{
	AxisWebDashboardParts *owner;
	GPtrArray             *parts;	/* of Part*, borrowed: the owner keeps them */
	gpointer              *figures;	/* one per part */
	HtmxRequest           *request;
	HtmxResponse          *deferred;
	gint64                 now_us;
} Task;

static void
task_free(Task *task)
{
	guint i;

	for (i = 0; i < task->parts->len; i++) {
		Part *part = g_ptr_array_index(task->parts, i);

		if (task->figures[i] != NULL && part->free_figures != NULL)
			part->free_figures(task->figures[i]);
	}
	g_free(task->figures);
	g_ptr_array_unref(task->parts);
	g_clear_object(&task->request);
	g_clear_object(&task->deferred);
	g_free(task);
}

static void
task_thread(
	GTask        *gtask,
	gpointer      source,
	gpointer      task_data,
	GCancellable *cancellable
){
	Task *task = task_data;
	guint i;

	for (i = 0; i < task->parts->len; i++) {
		Part *part = g_ptr_array_index(task->parts, i);

		if (part->collect != NULL)
			task->figures[i] = part->collect(part->user_data);
	}
	g_task_return_boolean(gtask, TRUE);
}

static void
task_finished(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	Task *task = user_data;
	HtmxResponse *response;

	task->owner->pending--;
	response = build_page(task->owner, task->parts, task->figures, task->request);
	htmx_response_complete(task->deferred, response);
	g_object_unref(response);
	task_free(task);
}

HtmxResponse *
axis_web_dashboard_handle(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	AxisWeb *web = user_data;
	AxisWebDashboardParts *self = axis_web_get_dashboard_parts(web);
	Task *task;
	GTask *gtask;
	HtmxResponse *returned;
	gboolean reads = FALSE;
	gboolean admin;
	guint i;

	/* The board's: from memory, at once, and no statement. */
	if (board_is_live(self)) {
		const gchar *part = htmx_request_get_query_param(request, "part");

		if (g_strcmp0(part, "cards") == 0) {
			/* What an open dashboard asks for after a reading: its cards, or
			 * nothing when it has these already. */
			const gchar *have = htmx_request_get_query_param(request, "have");
			HtmxResponse *response = htmx_response_new();
			g_autoptr(HtmxNode) cards = build_cards(self->parts, NULL, request);
			g_autofree gchar *mark = cards_mark(cards);

			if (have != NULL && strlen(have) == 16 && strncmp(have, mark, 16) == 0) {
				htmx_response_set_status(response, 204);
				htmx_response_set_content(response, "");
			} else {
				g_autofree gchar *markup = NULL;

				cards_go_live(self, cards);
				markup = htmx_element_render(HTMX_ELEMENT(cards));
				htmx_response_set_content(response, markup);
			}
			htmx_response_set_content_type(response, "text/html; charset=utf-8");
			return response;
		}
		/* Opened by a person: the figures are read again soon, and this page
		 * is told when they are. */
		axis_web_dashboard_parts_changed(self);
		return build_page(self, self->parts, NULL, request);
	}

	if (self->stopping || self->pending >= MAX_PENDING) {
		HtmxResponse *busy = axis_web_error_respond(request, 503, "Axis is busy",
			"Axis is busy. Try again in a few seconds.");

		htmx_response_add_header(busy, "Retry-After", "5");
		return busy;
	}

	task = g_new0(Task, 1);
	task->owner = self;
	task->parts = g_ptr_array_new();
	admin = axis_web_request_is_admin(request);
	for (i = 0; i < self->parts->len; i++) {
		Part *part = g_ptr_array_index(self->parts, i);

		/* Off with its module, and an administrators' part read for them
		 * only. */
		if (!part_is_live(part) || (part->admins_only && !admin))
			continue;
		g_ptr_array_add(task->parts, part);
		reads |= part->collect != NULL;
	}
	task->figures = g_new0(gpointer, MAX(task->parts->len, 1));
	task->now_us = g_get_real_time();

	/* Nothing to read: the page at once. */
	if (!reads) {
		HtmxResponse *response = build_page(self, task->parts, task->figures, request);

		task_free(task);
		return response;
	}

	task->request = g_object_ref(request);
	task->deferred = htmx_response_new_deferred();
	returned = g_object_ref(task->deferred);
	self->pending++;
	gtask = g_task_new(NULL, NULL, task_finished, task);
	g_task_set_task_data(gtask, task, NULL);
	g_task_run_in_thread(gtask, task_thread);
	g_object_unref(gtask);

	return returned;
}
