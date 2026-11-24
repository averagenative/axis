/*
 * axis-portal-wire.c - what axis-portal says to the daemon and how it reads
 * what the daemon answers
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "axis-portal-wire.h"

#include <string.h>

#include "api/axis-portal-proto.h"
#include "probe/axis-probe-json.h"

/* The largest time an answer may carry: the last second of the year 9999. */
#define MAX_TIME_S G_GINT64_CONSTANT(253402300799)

#define MAX_YEAR          9999
#define MAX_EPISODES      100000
#define MAX_COUNT         1000000000
#define USERNAME_MAX      64

/* ── Freeing ────────────────────────────────────────────────────── */

/* Wipes and frees a text that was a secret. */
static void
secret_free(gchar *text)
{
	volatile gchar *p = text;

	if (text == NULL)
		return;
	while (*p != '\0')
		*p++ = '\0';
	g_free(text);
}

void
axis_portal_wire_free(gchar *body)
{
	secret_free(body);
}

void
axis_portal_who_free(AxisPortalWho *who)
{
	if (who == NULL)
		return;
	g_free(who->name);
	g_free(who);
}

AxisPortalWho *
axis_portal_who_copy(const AxisPortalWho *who)
{
	AxisPortalWho *copy;

	if (who == NULL)
		return NULL;
	copy = g_new0(AxisPortalWho, 1);
	*copy = *who;
	copy->name = g_strdup(who->name);
	return copy;
}

void
axis_portal_hello_free(AxisPortalHello *hello)
{
	if (hello == NULL)
		return;
	g_strfreev(hello->lists);
	g_free(hello);
}

void
axis_portal_login_free(AxisPortalLogin *login)
{
	if (login == NULL)
		return;
	secret_free(login->proof);
	secret_free(login->device);
	g_free(login->name);
	g_free(login);
}

void
axis_portal_title_info_free(AxisPortalTitleInfo *title)
{
	if (title == NULL)
		return;
	g_free(title->name);
	g_free(title->overview);
	g_free(title->asked_by);
	g_clear_pointer(&title->seasons, g_array_unref);
	g_free(title);
}

void
axis_portal_titles_free(AxisPortalTitles *titles)
{
	if (titles == NULL)
		return;
	g_clear_pointer(&titles->titles, g_ptr_array_unref);
	g_free(titles);
}

void
axis_portal_request_info_free(AxisPortalRequestInfo *request)
{
	if (request == NULL)
		return;
	g_free(request->id);
	g_free(request->name);
	g_free(request->reason);
	g_clear_pointer(&request->seasons, g_array_unref);
	g_free(request);
}

void
axis_portal_requests_free(AxisPortalRequests *requests)
{
	if (requests == NULL)
		return;
	g_clear_pointer(&requests->requests, g_ptr_array_unref);
	g_free(requests);
}

void
axis_portal_asked_free(AxisPortalAsked *asked)
{
	if (asked == NULL)
		return;
	axis_portal_request_info_free(asked->request);
	g_clear_pointer(&asked->left_available, g_array_unref);
	g_clear_pointer(&asked->left_requested, g_array_unref);
	g_free(asked);
}

/* ── Checks ─────────────────────────────────────────────────────── */

/* Whether @text is exactly @length characters of [A-Za-z0-9_-]. */
static gboolean
is_token(
	const gchar *text,
	gsize        length
){
	gsize i;

	if (text == NULL || strlen(text) != length)
		return FALSE;
	for (i = 0; i < length; i++) {
		if (!g_ascii_isalnum(text[i]) && text[i] != '_' && text[i] != '-')
			return FALSE;
	}
	return TRUE;
}

gboolean
axis_portal_wire_is_proof(const gchar *text)
{
	return is_token(text, AXIS_PORTAL_PROOF_LEN);
}

gboolean
axis_portal_wire_is_request_id(const gchar *text)
{
	return is_token(text, AXIS_PORTAL_REQUEST_ID_LEN);
}

gboolean
axis_portal_wire_is_device(const gchar *text)
{
	gsize length;
	gsize i;

	if (text == NULL)
		return FALSE;
	length = strlen(text);
	if (length == 0 || length > AXIS_PORTAL_WIRE_DEVICE_MAX)
		return FALSE;
	for (i = 0; i < length; i++) {
		if (!g_ascii_isalnum(text[i]) && text[i] != '_' && text[i] != '-' && text[i] != '.')
			return FALSE;
	}
	return TRUE;
}

gboolean
axis_portal_wire_is_list(const gchar *text)
{
	gsize length;
	gsize i;

	if (text == NULL)
		return FALSE;
	length = strlen(text);
	if (length == 0 || length > AXIS_PORTAL_LIST_MAX_BYTES)
		return FALSE;
	for (i = 0; i < length; i++) {
		if (!g_ascii_islower(text[i]) && !g_ascii_isdigit(text[i]) && text[i] != '_' && text[i] != '-')
			return FALSE;
	}
	return TRUE;
}

/* An account's name, in the form the daemon gives every account: 1 to 64 of
 * [a-z0-9._-], the first a letter or a digit.  Nothing else is ever shown
 * or logged as a name. */
static gboolean
is_username(const gchar *text)
{
	gsize length;
	gsize i;

	if (text == NULL)
		return FALSE;
	length = strlen(text);
	if (length == 0 || length > USERNAME_MAX || !(g_ascii_islower(text[0]) || g_ascii_isdigit(text[0])))
		return FALSE;
	for (i = 0; i < length; i++) {
		if (!g_ascii_islower(text[i]) && !g_ascii_isdigit(text[i]) && text[i] != '.' && text[i] != '_' &&
		    text[i] != '-')
			return FALSE;
	}
	return TRUE;
}

/* A character no page shows: a control character, or one that changes the
 * direction or the joining of the text around it. */
static gboolean
is_hidden(gunichar c)
{
	return c < 0x20 || c == 0x7f || g_unichar_iscntrl(c) || (c >= 0x200b && c <= 0x200f) ||
		(c >= 0x202a && c <= 0x202e) || (c >= 0x2060 && c <= 0x2069) || c == 0xfeff || c == 0xfffe || c == 0xffff;
}

/* @text (valid UTF-8) cut to @max_chars, each hidden character a space. */
static gchar *
clean_text(
	const gchar *text,
	guint        max_chars
){
	GString *out = g_string_new(NULL);
	const gchar *p;
	guint n = 0;

	for (p = text; *p != '\0' && n < max_chars; p = g_utf8_next_char(p), n++) {
		gunichar c = g_utf8_get_char(p);

		if (is_hidden(c))
			g_string_append_c(out, ' ');
		else
			g_string_append_unichar(out, c);
	}
	return g_string_free(out, FALSE);
}

/* ── Reading: members ───────────────────────────────────────────── */

/* The answer's one object, or %NULL. */
static AxisJsonDoc *
parse_object(
	const gchar         *data,
	gsize                length,
	const AxisJsonNode **root
){
	AxisJsonDoc *doc;

	*root = NULL;
	if (data == NULL || length == 0 || length > AXIS_PORTAL_WIRE_MAX_ANSWER)
		return NULL;
	doc = axis_json_parse(data, length, NULL);
	if (doc == NULL)
		return NULL;
	*root = axis_json_doc_get_root(doc);
	if (axis_json_type(*root) != AXIS_JSON_OBJECT) {
		axis_json_doc_free(doc);
		*root = NULL;
		return NULL;
	}
	return doc;
}

/* Whether the members of @object are among @defined (%NULL-ended), each at
 * most once. */
static gboolean
only_members(
	const AxisJsonNode *object,
	const gchar *const *defined
){
	guint n = axis_json_length(object);
	guint i;
	guint j;

	if (axis_json_type(object) != AXIS_JSON_OBJECT || n > g_strv_length((gchar **)defined))
		return FALSE;
	for (i = 0; i < n; i++) {
		const gchar *name = axis_json_member_name(object, i);

		if (name == NULL || !g_strv_contains(defined, name))
			return FALSE;
		for (j = 0; j < i; j++) {
			if (strcmp(name, axis_json_member_name(object, j)) == 0)
				return FALSE;
		}
	}
	return TRUE;
}

static gboolean
has_member(
	const AxisJsonNode *object,
	const gchar        *name
){
	return axis_json_member(object, name) != NULL;
}

/* A member that is a text, or %NULL. */
static const gchar *
text_member(
	const AxisJsonNode *object,
	const gchar        *name
){
	const AxisJsonNode *node = axis_json_member(object, name);

	return axis_json_type(node) == AXIS_JSON_STRING ? axis_json_string(node) : NULL;
}

/* A member that is a whole number within its range: a text that holds one
 * is not. */
static gboolean
int_member(
	const AxisJsonNode *object,
	const gchar        *name,
	gint64              minimum,
	gint64              maximum,
	gint64             *value
){
	const AxisJsonNode *node = axis_json_member(object, name);

	return axis_json_type(node) == AXIS_JSON_NUMBER && axis_json_get_integer(node, minimum, maximum, value);
}

static gboolean
bool_member(
	const AxisJsonNode *object,
	const gchar        *name,
	gboolean           *value
){
	const AxisJsonNode *node = axis_json_member(object, name);

	return axis_json_type(node) == AXIS_JSON_BOOLEAN && axis_json_get_boolean(node, value);
}

/* The place of @word in @words (%NULL-ended), or -1. */
static gint
word_of(
	const gchar        *word,
	const gchar *const *words
){
	guint i;

	for (i = 0; word != NULL && words[i] != NULL; i++) {
		if (strcmp(word, words[i]) == 0)
			return (gint)i;
	}
	return -1;
}

static gboolean
kind_member(
	const AxisJsonNode *object,
	AxisPortalKind     *kind
){
	static const gchar *const kinds[] = { "movie", "series", NULL };
	gint found = word_of(text_member(object, "kind"), kinds);

	if (found < 0)
		return FALSE;
	*kind = (AxisPortalKind)found;
	return TRUE;
}

/* In the order of AxisPortalTitleState. */
static const gchar *const title_states[] = {
	"open", "available", "partly_available", "downloading", "approved", "requested", "declined", "closed", NULL
};

/* In the order of AxisPortalRequestState. */
static const gchar *const request_states[] = {
	"waiting", "approved", "downloading", "partly_available", "available", "declined", "withdrawn", "failed", NULL
};

static gboolean
quota_member(
	const AxisJsonNode *object,
	const gchar        *name,
	AxisPortalQuota    *quota
){
	static const gchar *const defined[] = { "limit", "left", "window_ends", NULL };
	const AxisJsonNode *node = axis_json_member(object, name);
	gint64 limit = 0;
	gint64 left = 0;
	gint64 ends = 0;

	if (!only_members(node, defined) || !int_member(node, "limit", 0, MAX_COUNT, &limit) ||
	    !int_member(node, "left", 0, MAX_COUNT, &left) || !int_member(node, "window_ends", 0, MAX_TIME_S, &ends))
		return FALSE;
	quota->limit = (guint)limit;
	quota->left = (guint)left;
	quota->window_ends = ends;
	return TRUE;
}

/* ── Reading: a title ───────────────────────────────────────────── */

static gboolean
read_season(
	const AxisJsonNode *node,
	AxisPortalSeasonInfo  *season
){
	static const gchar *const defined[] = { "number", "episodes", "aired", "state", NULL };
	gint64 number = 0;
	gint64 episodes = 0;
	gint64 aired = 0;
	gint state;

	if (!only_members(node, defined) || !int_member(node, "number", 0, AXIS_PORTAL_MAX_SEASON_NUMBER, &number) ||
	    !int_member(node, "episodes", 0, MAX_EPISODES, &episodes) || !int_member(node, "aired", 0, MAX_EPISODES, &aired))
		return FALSE;
	state = word_of(text_member(node, "state"), title_states);
	if (state < 0)
		return FALSE;
	season->number = (gint)number;
	season->episodes = (gint)episodes;
	season->aired = (gint)aired;
	season->state = (AxisPortalTitleState)state;
	return TRUE;
}

/* @with_seasons: the title is the one of the `title` operation, which may
 * carry "seasons"; a title of a list never does. */
static AxisPortalTitleInfo *
read_title(
	const AxisJsonNode *node,
	gboolean            with_seasons
){
	static const gchar *const defined[] = {
		"id", "kind", "name", "year", "overview", "has_poster", "state", "asked_by", "seasons", NULL
	};
	g_autoptr(AxisPortalTitleInfo) title = g_new0(AxisPortalTitleInfo, 1);
	const gchar *name;
	const gchar *overview;
	gint64 year = 0;
	gint state;
	guint i;
	guint j;

	if (!only_members(node, defined) || !int_member(node, "id", 1, AXIS_PORTAL_MAX_ID, &title->id) ||
	    !kind_member(node, &title->kind) || !int_member(node, "year", 0, MAX_YEAR, &year) ||
	    !bool_member(node, "has_poster", &title->has_poster))
		return NULL;
	name = text_member(node, "name");
	overview = text_member(node, "overview");
	state = word_of(text_member(node, "state"), title_states);
	if (name == NULL || overview == NULL || state < 0)
		return NULL;
	title->name = clean_text(name, AXIS_PORTAL_WIRE_NAME_MAX_CHARS);
	title->overview = clean_text(overview, AXIS_PORTAL_WIRE_OVERVIEW_MAX_CHARS);
	title->year = (gint)year;
	title->state = (AxisPortalTitleState)state;
	if (has_member(node, "asked_by")) {
		const gchar *asked_by = text_member(node, "asked_by");

		if (!is_username(asked_by))
			return NULL;
		title->asked_by = g_strdup(asked_by);
	}
	if (has_member(node, "seasons")) {
		const AxisJsonNode *seasons = axis_json_member(node, "seasons");

		if (!with_seasons || title->kind != AXIS_PORTAL_KIND_IS_SERIES || axis_json_type(seasons) != AXIS_JSON_ARRAY ||
		    axis_json_length(seasons) > AXIS_PORTAL_WIRE_MAX_TITLE_SEASONS)
			return NULL;
		title->has_seasons = TRUE;
		title->seasons = g_array_new(FALSE, TRUE, sizeof(AxisPortalSeasonInfo));
		for (i = 0; i < axis_json_length(seasons); i++) {
			AxisPortalSeasonInfo season;

			if (!read_season(axis_json_element(seasons, i), &season))
				return NULL;
			/* A season is listed once: a form's box stands for one. */
			for (j = 0; j < title->seasons->len; j++) {
				if (g_array_index(title->seasons, AxisPortalSeasonInfo, j).number == season.number)
					return NULL;
			}
			g_array_append_val(title->seasons, season);
		}
	}
	return g_steal_pointer(&title);
}

AxisPortalTitleInfo *
axis_portal_wire_read_title(
	const gchar *data,
	gsize        length
){
	static const gchar *const defined[] = { "title", NULL };
	const AxisJsonNode *root;
	g_autoptr(AxisJsonDoc) doc = parse_object(data, length, &root);

	if (doc == NULL || !only_members(root, defined) || !has_member(root, "title"))
		return NULL;
	return read_title(axis_json_member(root, "title"), TRUE);
}

AxisPortalTitles *
axis_portal_wire_read_titles(
	const gchar *data,
	gsize        length,
	gboolean     search
){
	static const gchar *const of_search[] = { "titles", "page", "more", "stale", NULL };
	static const gchar *const of_browse[] = { "titles", "page", "more", NULL };
	const AxisJsonNode *root;
	const AxisJsonNode *list;
	g_autoptr(AxisJsonDoc) doc = parse_object(data, length, &root);
	g_autoptr(AxisPortalTitles) titles = NULL;
	gint64 page = 0;
	guint i;

	if (doc == NULL || !only_members(root, search ? of_search : of_browse))
		return NULL;
	titles = g_new0(AxisPortalTitles, 1);
	titles->titles = g_ptr_array_new_with_free_func((GDestroyNotify)axis_portal_title_info_free);
	list = axis_json_member(root, "titles");
	if (axis_json_type(list) != AXIS_JSON_ARRAY || axis_json_length(list) > AXIS_PORTAL_WIRE_MAX_TITLES ||
	    !int_member(root, "page", 1, AXIS_PORTAL_MAX_PAGE, &page) || !bool_member(root, "more", &titles->more) ||
	    (search && !bool_member(root, "stale", &titles->stale)))
		return NULL;
	titles->page = (guint)page;
	for (i = 0; i < axis_json_length(list); i++) {
		AxisPortalTitleInfo *title = read_title(axis_json_element(list, i), FALSE);

		if (title == NULL)
			return NULL;
		g_ptr_array_add(titles->titles, title);
	}
	return g_steal_pointer(&titles);
}

/* ── Reading: a request ─────────────────────────────────────────── */

static AxisPortalRequestInfo *
read_request(const AxisJsonNode *node)
{
	static const gchar *const defined[] = {
		"id", "title", "kind", "name", "year", "seasons", "state", "have", "of", "cause", "reason", "mine",
		"asked_at", "changed_at", NULL
	};
	static const gchar *const causes[] = { "", "not_added", "download_failed", "needs_admin", "removed", NULL };
	static const gchar *const mine[] = { "followed", "made", NULL };
	g_autoptr(AxisPortalRequestInfo) request = g_new0(AxisPortalRequestInfo, 1);
	const AxisJsonNode *seasons;
	const gchar *id;
	const gchar *name;
	gint64 year = 0;
	gint state;
	gint whose;
	guint i;

	if (!only_members(node, defined) || !int_member(node, "title", 1, AXIS_PORTAL_MAX_ID, &request->title) ||
	    !kind_member(node, &request->kind) || !int_member(node, "year", 0, MAX_YEAR, &year) ||
	    !int_member(node, "asked_at", 0, MAX_TIME_S, &request->asked_at) ||
	    !int_member(node, "changed_at", 0, MAX_TIME_S, &request->changed_at))
		return NULL;
	id = text_member(node, "id");
	name = text_member(node, "name");
	state = word_of(text_member(node, "state"), request_states);
	whose = word_of(text_member(node, "mine"), mine);
	seasons = axis_json_member(node, "seasons");
	if (!axis_portal_wire_is_request_id(id) || name == NULL || state < 0 || whose < 0 ||
	    axis_json_type(seasons) != AXIS_JSON_ARRAY || axis_json_length(seasons) > AXIS_PORTAL_MAX_SEASONS)
		return NULL;
	request->id = g_strdup(id);
	request->name = clean_text(name, AXIS_PORTAL_WIRE_NAME_MAX_CHARS);
	request->year = (gint)year;
	request->state = (AxisPortalRequestState)state;
	request->made = whose == 1;
	request->seasons = g_array_new(FALSE, TRUE, sizeof(gint));
	for (i = 0; i < axis_json_length(seasons); i++) {
		const AxisJsonNode *item = axis_json_element(seasons, i);
		gint64 number = 0;
		gint season;

		if (axis_json_type(item) != AXIS_JSON_NUMBER ||
		    !axis_json_get_integer(item, 0, AXIS_PORTAL_MAX_SEASON_NUMBER, &number))
			return NULL;
		season = (gint)number;
		g_array_append_val(request->seasons, season);
	}
	/* The episode counts come together or not at all. */
	if (has_member(node, "have") || has_member(node, "of")) {
		gint64 have = 0;
		gint64 of = 0;

		if (!int_member(node, "have", 0, MAX_COUNT, &have) || !int_member(node, "of", 0, MAX_COUNT, &of))
			return NULL;
		request->counts = TRUE;
		request->have = (guint)have;
		request->of = (guint)of;
	}
	if (has_member(node, "cause")) {
		gint cause = word_of(text_member(node, "cause"), causes);

		if (cause <= 0)
			return NULL;
		request->cause = (AxisPortalCause)cause;
	}
	if (has_member(node, "reason")) {
		const gchar *reason = text_member(node, "reason");

		if (reason == NULL)
			return NULL;
		request->reason = clean_text(reason, AXIS_PORTAL_WIRE_REASON_MAX_CHARS);
	}
	return g_steal_pointer(&request);
}

AxisPortalRequests *
axis_portal_wire_read_requests(
	const gchar *data,
	gsize        length
){
	static const gchar *const defined[] = { "requests", "page", "more", NULL };
	const AxisJsonNode *root;
	const AxisJsonNode *list;
	g_autoptr(AxisJsonDoc) doc = parse_object(data, length, &root);
	g_autoptr(AxisPortalRequests) requests = NULL;
	gint64 page = 0;
	guint i;

	if (doc == NULL || !only_members(root, defined))
		return NULL;
	requests = g_new0(AxisPortalRequests, 1);
	requests->requests = g_ptr_array_new_with_free_func((GDestroyNotify)axis_portal_request_info_free);
	list = axis_json_member(root, "requests");
	if (axis_json_type(list) != AXIS_JSON_ARRAY || axis_json_length(list) > AXIS_PORTAL_WIRE_MAX_REQUESTS ||
	    !int_member(root, "page", 1, AXIS_PORTAL_MAX_PAGE, &page) || !bool_member(root, "more", &requests->more))
		return NULL;
	requests->page = (guint)page;
	for (i = 0; i < axis_json_length(list); i++) {
		AxisPortalRequestInfo *request = read_request(axis_json_element(list, i));

		if (request == NULL)
			return NULL;
		g_ptr_array_add(requests->requests, request);
	}
	return g_steal_pointer(&requests);
}

AxisPortalAsked *
axis_portal_wire_read_asked(
	const gchar *data,
	gsize        length
){
	static const gchar *const defined[] = { "outcome", "request", "left_out", "followed", "movies", "seasons", NULL };
	static const gchar *const outcomes[] = { "made", "followed", "available", "unchanged", NULL };
	static const gchar *const of_left[] = { "season", "why", NULL };
	static const gchar *const whys[] = { "available", "requested", NULL };
	const AxisJsonNode *root;
	const AxisJsonNode *left;
	g_autoptr(AxisJsonDoc) doc = parse_object(data, length, &root);
	g_autoptr(AxisPortalAsked) asked = NULL;
	gint64 followed = 0;
	gint outcome;
	guint i;

	if (doc == NULL || !only_members(root, defined))
		return NULL;
	asked = g_new0(AxisPortalAsked, 1);
	asked->left_available = g_array_new(FALSE, TRUE, sizeof(gint));
	asked->left_requested = g_array_new(FALSE, TRUE, sizeof(gint));
	outcome = word_of(text_member(root, "outcome"), outcomes);
	left = axis_json_member(root, "left_out");
	if (outcome < 0 || axis_json_type(left) != AXIS_JSON_ARRAY || axis_json_length(left) > AXIS_PORTAL_MAX_SEASONS ||
	    !int_member(root, "followed", 0, AXIS_PORTAL_MAX_SEASONS, &followed) ||
	    !quota_member(root, "movies", &asked->movies) || !quota_member(root, "seasons", &asked->seasons))
		return NULL;
	asked->outcome = (AxisPortalOutcome)outcome;
	asked->followed = (guint)followed;
	for (i = 0; i < axis_json_length(left); i++) {
		const AxisJsonNode *item = axis_json_element(left, i);
		gint64 number = 0;
		gint season;
		gint why;

		if (!only_members(item, of_left) || !int_member(item, "season", 0, AXIS_PORTAL_MAX_SEASON_NUMBER, &number))
			return NULL;
		why = word_of(text_member(item, "why"), whys);
		if (why < 0)
			return NULL;
		season = (gint)number;
		g_array_append_val(why == 0 ? asked->left_available : asked->left_requested, season);
	}
	if (has_member(root, "request")) {
		asked->request = read_request(axis_json_member(root, "request"));
		if (asked->request == NULL)
			return NULL;
	}
	return g_steal_pointer(&asked);
}

/* ── Reading: the rest ──────────────────────────────────────────── */

AxisPortalHello *
axis_portal_wire_read_hello(
	const gchar *data,
	gsize        length
){
	static const gchar *const defined[] = { "protocol", "kinds", "lists", "show_names", NULL };
	const AxisJsonNode *root;
	const AxisJsonNode *kinds;
	const AxisJsonNode *lists;
	g_autoptr(AxisJsonDoc) doc = parse_object(data, length, &root);
	g_autoptr(AxisPortalHello) hello = NULL;
	g_autoptr(GPtrArray) names = g_ptr_array_new_with_free_func(g_free);
	gint64 protocol = 0;
	guint i;
	guint j;

	if (doc == NULL || !only_members(root, defined))
		return NULL;
	hello = g_new0(AxisPortalHello, 1);
	kinds = axis_json_member(root, "kinds");
	lists = axis_json_member(root, "lists");
	/* A daemon that speaks another protocol is one this portal cannot
	 * read. */
	if (!int_member(root, "protocol", AXIS_PORTAL_PROTOCOL, AXIS_PORTAL_PROTOCOL, &protocol) ||
	    axis_json_type(kinds) != AXIS_JSON_ARRAY || axis_json_length(kinds) > 2 ||
	    axis_json_type(lists) != AXIS_JSON_ARRAY || axis_json_length(lists) > AXIS_PORTAL_WIRE_MAX_LISTS ||
	    !bool_member(root, "show_names", &hello->show_names))
		return NULL;
	for (i = 0; i < axis_json_length(kinds); i++) {
		const AxisJsonNode *item = axis_json_element(kinds, i);
		const gchar *kind = axis_json_type(item) == AXIS_JSON_STRING ? axis_json_string(item) : NULL;

		if (g_strcmp0(kind, "movie") == 0 && !hello->movies)
			hello->movies = TRUE;
		else if (g_strcmp0(kind, "series") == 0 && !hello->series)
			hello->series = TRUE;
		else
			return NULL;
	}
	for (i = 0; i < axis_json_length(lists); i++) {
		const AxisJsonNode *item = axis_json_element(lists, i);
		const gchar *list = axis_json_type(item) == AXIS_JSON_STRING ? axis_json_string(item) : NULL;

		if (!axis_portal_wire_is_list(list))
			return NULL;
		for (j = 0; j < names->len; j++) {
			if (strcmp(g_ptr_array_index(names, j), list) == 0)
				return NULL;
		}
		g_ptr_array_add(names, g_strdup(list));
	}
	g_ptr_array_add(names, NULL);
	hello->lists = (gchar **)g_ptr_array_free(g_steal_pointer(&names), FALSE);
	return g_steal_pointer(&hello);
}

AxisPortalLogin *
axis_portal_wire_read_login(
	const gchar *data,
	gsize        length
){
	static const gchar *const defined[] = { "proof", "name", "expires_in", "device", NULL };
	const AxisJsonNode *root;
	g_autoptr(AxisJsonDoc) doc = parse_object(data, length, &root);
	g_autoptr(AxisPortalLogin) login = NULL;
	const gchar *proof;
	const gchar *name;
	const gchar *device;

	if (doc == NULL || !only_members(root, defined))
		return NULL;
	login = g_new0(AxisPortalLogin, 1);
	proof = text_member(root, "proof");
	name = text_member(root, "name");
	device = text_member(root, "device");
	if (!axis_portal_wire_is_proof(proof) || !is_username(name) || device == NULL ||
	    !int_member(root, "expires_in", 1, AXIS_PORTAL_WIRE_MAX_EXPIRES_S, &login->expires_in))
		return NULL;
	/* No device value is an empty one; any other must have the form of one. */
	if (device[0] != '\0' && !axis_portal_wire_is_device(device))
		return NULL;
	login->proof = g_strdup(proof);
	login->name = g_strdup(name);
	login->device = device[0] != '\0' ? g_strdup(device) : NULL;
	return g_steal_pointer(&login);
}

AxisPortalWho *
axis_portal_wire_read_whoami(
	const gchar *data,
	gsize        length
){
	static const gchar *const defined[] = { "name", "movies", "seasons", NULL };
	const AxisJsonNode *root;
	g_autoptr(AxisJsonDoc) doc = parse_object(data, length, &root);
	g_autoptr(AxisPortalWho) who = NULL;
	const gchar *name;

	if (doc == NULL || !only_members(root, defined))
		return NULL;
	who = g_new0(AxisPortalWho, 1);
	name = text_member(root, "name");
	if (!is_username(name) || !quota_member(root, "movies", &who->movies) ||
	    !quota_member(root, "seasons", &who->seasons))
		return NULL;
	who->name = g_strdup(name);
	return g_steal_pointer(&who);
}

gboolean
axis_portal_wire_read_empty(
	const gchar *data,
	gsize        length
){
	const AxisJsonNode *root;
	g_autoptr(AxisJsonDoc) doc = parse_object(data, length, &root);

	return doc != NULL && axis_json_length(root) == 0;
}

gboolean
axis_portal_wire_read_refusal(
	const gchar       *data,
	gsize              length,
	AxisPortalRefusal *refusal
){
	static const gchar *const defined[] = { "error", "message", "wait", NULL };
	/* The daemon's words, with what each is here. */
	static const struct
	{
		const gchar          *word;
		AxisPortalRefusedWhy  why;
	} words[] = {
		{ "proof", AXIS_PORTAL_REFUSED_PROOF },
		{ "login", AXIS_PORTAL_REFUSED_LOGIN },
		{ "unauthorized", AXIS_PORTAL_REFUSED_TOKEN },
		{ "throttled", AXIS_PORTAL_REFUSED_THROTTLED },
		{ "quota", AXIS_PORTAL_REFUSED_QUOTA },
		{ "too-many", AXIS_PORTAL_REFUSED_TOO_MANY },
		{ "portal-limit", AXIS_PORTAL_REFUSED_PORTAL_LIMIT },
		{ "portal-share", AXIS_PORTAL_REFUSED_PORTAL_SHARE },
		{ "search-limit", AXIS_PORTAL_REFUSED_SEARCH_LIMIT },
		{ "person-limit", AXIS_PORTAL_REFUSED_PERSON_LIMIT },
		{ "token-limit", AXIS_PORTAL_REFUSED_TOKEN_LIMIT },
		{ "not-found", AXIS_PORTAL_REFUSED_NOT_FOUND },
		{ "closed", AXIS_PORTAL_REFUSED_CLOSED },
		{ "kind-off", AXIS_PORTAL_REFUSED_KIND_OFF },
		{ "decided", AXIS_PORTAL_REFUSED_DECIDED },
		{ "invalid", AXIS_PORTAL_REFUSED_INVALID },
		{ "malformed", AXIS_PORTAL_REFUSED_INVALID },
		{ "busy", AXIS_PORTAL_REFUSED_BUSY }
	};
	const AxisJsonNode *root;
	g_autoptr(AxisJsonDoc) doc = NULL;
	const gchar *word;
	guint i;

	refusal->why = AXIS_PORTAL_REFUSED_OTHER;
	refusal->wait_s = 0;
	doc = parse_object(data, length, &root);
	if (doc == NULL || !only_members(root, defined))
		return FALSE;
	word = text_member(root, "error");
	/* The sentence must be there and be a text; it is not kept. */
	if (word == NULL || text_member(root, "message") == NULL)
		return FALSE;
	if (has_member(root, "wait") && !int_member(root, "wait", 1, AXIS_PORTAL_WIRE_MAX_WAIT_S, &refusal->wait_s)) {
		refusal->wait_s = 0;
		return FALSE;
	}
	for (i = 0; i < G_N_ELEMENTS(words); i++) {
		if (strcmp(word, words[i].word) == 0)
			refusal->why = words[i].why;
	}
	return TRUE;
}

/* ── Writing ────────────────────────────────────────────────────── */

/* @text as a JSON string.  The caller has checked that it is valid UTF-8. */
static void
w_text(
	GString     *out,
	const gchar *text
){
	const guchar *p;

	g_string_append_c(out, '"');
	for (p = (const guchar *)text; *p != '\0'; p++) {
		if (*p == '"')
			g_string_append(out, "\\\"");
		else if (*p == '\\')
			g_string_append(out, "\\\\");
		else if (*p < 0x20 || *p == 0x7f)
			g_string_append_printf(out, "\\u%04x", (guint)*p);
		else
			g_string_append_c(out, (gchar)*p);
	}
	g_string_append_c(out, '"');
}

/* A text of 1 to @max_bytes bytes of valid UTF-8. */
static gboolean
is_text(
	const gchar *text,
	gsize        max_bytes
){
	gsize length;

	if (text == NULL)
		return FALSE;
	length = strlen(text);
	return length > 0 && length <= max_bytes && g_utf8_validate(text, (gssize)length, NULL);
}

/* The body so far is a secret from here on; nothing is sent. */
static gchar *
give_up(GString *out)
{
	secret_free(g_string_free(out, FALSE));
	return NULL;
}

/* Begins a body with the proof, or gives %NULL for a text that is not one. */
static GString *
begin_with_proof(const gchar *proof)
{
	GString *out;

	if (!axis_portal_wire_is_proof(proof))
		return NULL;
	out = g_string_new("{\"proof\":");
	w_text(out, proof);
	return out;
}

gchar *
axis_portal_wire_login(
	const gchar *username,
	const gchar *password,
	const gchar *client_address,
	const gchar *user_agent,
	const gchar *device
){
	GString *out;
	const gchar *p;

	if (!is_text(username, AXIS_PORTAL_USERNAME_MAX_BYTES) || !is_text(password, AXIS_PORTAL_PASSWORD_MAX_BYTES) ||
	    !is_text(client_address, AXIS_PORTAL_ADDRESS_MAX_BYTES) || !is_text(user_agent, AXIS_PORTAL_AGENT_MAX_BYTES) ||
	    (device != NULL && !axis_portal_wire_is_device(device)))
		return NULL;
	/* An address is digits, hexadecimal letters, dots and colons; a user
	 * agent is printable ASCII (the caller made it so). */
	for (p = client_address; *p != '\0'; p++) {
		if (!g_ascii_isxdigit(*p) && *p != '.' && *p != ':')
			return NULL;
	}
	for (p = user_agent; *p != '\0'; p++) {
		if (*p < 0x20 || *p > 0x7e)
			return NULL;
	}
	out = g_string_new("{\"username\":");
	w_text(out, username);
	g_string_append(out, ",\"password\":");
	w_text(out, password);
	g_string_append(out, ",\"client_address\":");
	w_text(out, client_address);
	g_string_append(out, ",\"user_agent\":");
	w_text(out, user_agent);
	if (device != NULL) {
		g_string_append(out, ",\"device\":");
		w_text(out, device);
	}
	g_string_append_c(out, '}');
	return g_string_free(out, FALSE);
}

gchar *
axis_portal_wire_proof(const gchar *proof)
{
	GString *out = begin_with_proof(proof);

	if (out == NULL)
		return NULL;
	g_string_append_c(out, '}');
	return g_string_free(out, FALSE);
}

gchar *
axis_portal_wire_search(
	const gchar *proof,
	const gchar *query,
	gint         kind,
	guint        page
){
	GString *out;
	const gchar *p;

	if (!is_text(query, AXIS_PORTAL_QUERY_MAX_CHARS * 4) || g_utf8_strlen(query, -1) > AXIS_PORTAL_QUERY_MAX_CHARS ||
	    kind < -1 || kind > 1 || page < 1 || page > AXIS_PORTAL_SEARCH_MAX_PAGE)
		return NULL;
	for (p = query; *p != '\0'; p = g_utf8_next_char(p)) {
		gunichar c = g_utf8_get_char(p);

		if (c < 0x20 || c == 0x7f || g_unichar_iscntrl(c))
			return NULL;
	}
	out = begin_with_proof(proof);
	if (out == NULL)
		return NULL;
	g_string_append(out, ",\"query\":");
	w_text(out, query);
	if (kind >= 0)
		g_string_append_printf(out, ",\"kind\":\"%s\"", kind == 1 ? "series" : "movie");
	g_string_append_printf(out, ",\"page\":%u}", page);
	return g_string_free(out, FALSE);
}

gchar *
axis_portal_wire_browse(
	const gchar    *proof,
	const gchar    *list,
	AxisPortalKind  kind,
	guint           page
){
	GString *out;

	if (!axis_portal_wire_is_list(list) || (kind != AXIS_PORTAL_KIND_IS_MOVIE && kind != AXIS_PORTAL_KIND_IS_SERIES) ||
	    page < 1 || page > AXIS_PORTAL_MAX_PAGE)
		return NULL;
	out = begin_with_proof(proof);
	if (out == NULL)
		return NULL;
	g_string_append(out, ",\"list\":");
	w_text(out, list);
	g_string_append_printf(out, ",\"kind\":\"%s\",\"page\":%u}", kind == AXIS_PORTAL_KIND_IS_SERIES ? "series" : "movie",
		page);
	return g_string_free(out, FALSE);
}

gchar *
axis_portal_wire_title(
	const gchar *proof,
	gint64       id
){
	GString *out;

	if (id < 1 || id > AXIS_PORTAL_MAX_ID)
		return NULL;
	out = begin_with_proof(proof);
	if (out == NULL)
		return NULL;
	g_string_append_printf(out, ",\"id\":%" G_GINT64_FORMAT "}", id);
	return g_string_free(out, FALSE);
}

gchar *
axis_portal_wire_request(
	const gchar    *proof,
	gint64          id,
	AxisPortalKind  kind,
	AxisPortalAsk   ask,
	const gint     *picked,
	guint           n_picked
){
	GString *out;
	guint i;
	guint j;

	if (id < 1 || id > AXIS_PORTAL_MAX_ID || (kind != AXIS_PORTAL_KIND_IS_MOVIE && kind != AXIS_PORTAL_KIND_IS_SERIES))
		return NULL;
	out = begin_with_proof(proof);
	if (out == NULL)
		return NULL;
	g_string_append_printf(out, ",\"id\":%" G_GINT64_FORMAT ",\"kind\":\"%s\"", id,
		kind == AXIS_PORTAL_KIND_IS_SERIES ? "series" : "movie");
	/* A movie has no seasons to say; a series says which. */
	if (kind == AXIS_PORTAL_KIND_IS_SERIES) {
		switch (ask) {
		case AXIS_PORTAL_ASK_ALL:
			g_string_append(out, ",\"seasons\":\"all\"");
			break;
		case AXIS_PORTAL_ASK_FIRST:
			g_string_append(out, ",\"seasons\":\"first\"");
			break;
		case AXIS_PORTAL_ASK_LATEST:
			g_string_append(out, ",\"seasons\":\"latest\"");
			break;
		case AXIS_PORTAL_ASK_PICKED:
			if (picked == NULL || n_picked == 0 || n_picked > AXIS_PORTAL_MAX_SEASONS)
				return give_up(out);
			g_string_append(out, ",\"seasons\":[");
			for (i = 0; i < n_picked; i++) {
				if (picked[i] < 0 || picked[i] > AXIS_PORTAL_MAX_SEASON_NUMBER)
					return give_up(out);
				for (j = 0; j < i; j++) {
					if (picked[j] == picked[i])
						return give_up(out);
				}
				g_string_append_printf(out, "%s%d", i > 0 ? "," : "", picked[i]);
			}
			g_string_append_c(out, ']');
			break;
		default:
			return give_up(out);
		}
	}
	g_string_append_c(out, '}');
	return g_string_free(out, FALSE);
}

gchar *
axis_portal_wire_requests(
	const gchar *proof,
	guint        page
){
	GString *out;

	if (page < 1 || page > AXIS_PORTAL_MAX_PAGE)
		return NULL;
	out = begin_with_proof(proof);
	if (out == NULL)
		return NULL;
	g_string_append_printf(out, ",\"page\":%u}", page);
	return g_string_free(out, FALSE);
}

gchar *
axis_portal_wire_withdraw(
	const gchar *proof,
	const gchar *request_id
){
	GString *out;

	if (!axis_portal_wire_is_request_id(request_id))
		return NULL;
	out = begin_with_proof(proof);
	if (out == NULL)
		return NULL;
	g_string_append(out, ",\"request\":");
	w_text(out, request_id);
	g_string_append_c(out, '}');
	return g_string_free(out, FALSE);
}
