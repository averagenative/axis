/*
 * axis-portal-proto.h - the portal API's messages: the one reader of an
 * operation's body and the one writer of its answers
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * (add-request-portal, slice P2.)  The portal API is a closed list of eleven
 * operations, each a POST of one JSON object to AXIS_PORTAL_API_PREFIX<name>
 * on the daemon's portal listener (api/axis-portal-api.h).  What arrives is a
 * claim from a party that may be in the wrong hands, so:
 *
 *  - a body is read by axis_portal_request_parse() and by nothing else: one
 *    JSON object of at most AXIS_PORTAL_MAX_REQUEST_BYTES, with exactly the
 *    members its operation defines, each once, each of its type and within
 *    its length.  A member the operation does not define is a refusal of the
 *    whole body, whatever its name: there is no member that names a person, a
 *    folder, a profile or an address to fetch;
 *  - an answer is written by the functions below and by nothing else: the
 *    members of a title, of a request and of a quota are these and no others,
 *    so that what an answer can carry is read off this one file.
 *
 * GLib only, no I/O: read by the fuzz target (tests/fuzz/fuzz-portal-body.c).
 */

#ifndef AXIS_PORTAL_PROTO_H
#define AXIS_PORTAL_PROTO_H

#include <glib.h>

G_BEGIN_DECLS

#define AXIS_PORTAL_API_PREFIX          "/api/v1/portal/"
#define AXIS_PORTAL_PROTOCOL            1

/* The largest body of an operation. */
#define AXIS_PORTAL_MAX_REQUEST_BYTES   (16 * 1024)

#define AXIS_PORTAL_QUERY_MAX_CHARS     100
#define AXIS_PORTAL_OVERVIEW_MAX_CHARS  600
#define AXIS_PORTAL_USERNAME_MAX_BYTES  256
#define AXIS_PORTAL_PASSWORD_MAX_BYTES  1024
#define AXIS_PORTAL_ADDRESS_MAX_BYTES   64
#define AXIS_PORTAL_AGENT_MAX_BYTES     512
#define AXIS_PORTAL_DEVICE_MAX_BYTES    160
#define AXIS_PORTAL_LIST_MAX_BYTES      16
#define AXIS_PORTAL_MAX_ID              G_GINT64_CONSTANT(999999999999999)
#define AXIS_PORTAL_MAX_PAGE            100
/* A search has no more pages than the engine's search asks the provider for
 * (axis_meta_search_page()). */
#define AXIS_PORTAL_SEARCH_MAX_PAGE     2
#define AXIS_PORTAL_MAX_SEASONS         100
#define AXIS_PORTAL_MAX_SEASON_NUMBER   10000

/* A session proof and a request's identifier, as the daemon issues them. */
#define AXIS_PORTAL_PROOF_LEN           43
#define AXIS_PORTAL_REQUEST_ID_LEN      22

typedef enum
{
	AXIS_PORTAL_HELLO,
	AXIS_PORTAL_LOGIN,
	AXIS_PORTAL_LOGOUT,
	AXIS_PORTAL_WHOAMI,
	AXIS_PORTAL_SEARCH,
	AXIS_PORTAL_BROWSE,
	AXIS_PORTAL_TITLE,
	AXIS_PORTAL_ART,
	AXIS_PORTAL_REQUEST,
	AXIS_PORTAL_REQUESTS,
	AXIS_PORTAL_WITHDRAW
} AxisPortalOp;

#define AXIS_PORTAL_N_OPS 11

/* "hello", "login", ...: the last part of the operation's path. */
const gchar *
axis_portal_op_name(AxisPortalOp op);

gboolean
axis_portal_op_from_name(
	const gchar  *name,
	AxisPortalOp *op
);

/* Whether @op is for a person: every one but hello and login. */
gboolean
axis_portal_op_needs_proof(AxisPortalOp op);

/* A kind as the API names it: "movie" (0) or "series" (1). */
#define AXIS_PORTAL_KIND_MOVIE  0
#define AXIS_PORTAL_KIND_SERIES 1

/* How a series' seasons are chosen: "all", "first", "latest", or a list of
 * numbers (picked). */
#define AXIS_PORTAL_SEASONS_ALL    0
#define AXIS_PORTAL_SEASONS_FIRST  1
#define AXIS_PORTAL_SEASONS_LATEST 2
#define AXIS_PORTAL_SEASONS_PICKED 3

/**
 * AxisPortalRequest:
 * @op: the operation the body was read for
 * @proof: (nullable): the session proof, when the body carries a well-formed
 *   one (a text that is not well-formed is no proof); wiped when freed
 * @valid: every member is one the operation defines, once, of its type and
 *   within its length, and every member it requires is there.  The members
 *   below are set only then
 * @problem: (nullable): else why not: a short sentence that holds nothing of
 *   the body
 * @username, @password, @client_address, @user_agent, @device: login
 *   (@device nullable; @password wiped when freed)
 * @query: search
 * @has_kind, @kind: search (optional), browse and request
 * @page: 1 when the body gives none; a search's at most
 *   %AXIS_PORTAL_SEARCH_MAX_PAGE
 * @list: browse
 * @id: a title: title, art, request
 * @seasons: request, for a series: AXIS_PORTAL_SEASONS_*; -1 for a movie
 * @picked, @n_picked: the numbers of AXIS_PORTAL_SEASONS_PICKED, each once
 * @request: withdraw: a request's identifier
 */
typedef struct
{
	AxisPortalOp  op;
	gchar        *proof;
	gboolean      valid;
	gchar        *problem;
	gchar        *username;
	gchar        *password;
	gchar        *client_address;
	gchar        *user_agent;
	gchar        *device;
	gchar        *query;
	gboolean      has_kind;
	gint          kind;
	guint         page;
	gchar        *list;
	gint64        id;
	gint          seasons;
	gint          picked[AXIS_PORTAL_MAX_SEASONS];
	guint         n_picked;
	gchar        *request;
} AxisPortalRequest;

/**
 * axis_portal_request_parse:
 * @op: the operation
 * @data: (nullable): the body
 * @length: its length
 * @problem: (out) (optional) (transfer full): why %NULL was returned
 *
 * The one reader of an operation's body.
 *
 * Returns: (transfer full) (nullable): %NULL when the body is not one JSON
 *   object within the limits (nothing of it can be believed, not even a
 *   proof); else the request, whose @valid says whether its members are the
 *   operation's
 */
AxisPortalRequest *
axis_portal_request_parse(
	AxisPortalOp   op,
	const gchar   *data,
	gsize          length,
	gchar        **problem
);

void
axis_portal_request_free(AxisPortalRequest *request);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisPortalRequest, axis_portal_request_free)

/* ── Answers ────────────────────────────────────────────────────── */

/* {"error": @word, "message": @message}, and "wait" (seconds) when @wait_s
 * is above 0.  Transfer full. */
gchar *
axis_portal_error_to_json(
	const gchar *word,
	const gchar *message
);

gchar *
axis_portal_error_wait_to_json(
	const gchar *word,
	const gchar *message,
	gint64       wait_s
);

typedef struct
{
	gint         number;
	gint         episodes;
	gint         aired;
	const gchar *state;
} AxisPortalSeason;

/**
 * AxisPortalTitle:
 *
 * A title as an answer carries it: "id", "kind", "name", "year", "overview"
 * (cut to %AXIS_PORTAL_OVERVIEW_MAX_CHARS), "has_poster", "state", "asked_by"
 * (only when @asked_by is given) and, when @with_seasons, "seasons" (each
 * "number", "episodes", "aired", "state").  Nothing else.
 */
typedef struct
{
	gint64                  id;
	const gchar            *kind;
	const gchar            *name;
	gint                    year;
	const gchar            *overview;
	gboolean                has_poster;
	const gchar            *state;
	const gchar            *asked_by;
	gboolean                with_seasons;
	const AxisPortalSeason *seasons;
	guint                   n_seasons;
} AxisPortalTitle;

/**
 * AxisPortalRequestView:
 *
 * A request as an answer carries it: "id", "title" (the title's id), "kind",
 * "name", "year", "seasons", "state", "have" and "of" (when @counts), "cause"
 * (when given), "reason" (when given), "mine", "asked_at" and "changed_at"
 * (seconds since 1970).  Nothing else.
 */
typedef struct
{
	const gchar *id;
	gint64       title;
	const gchar *kind;
	const gchar *name;
	gint         year;
	const gint  *seasons;
	guint        n_seasons;
	const gchar *state;
	gboolean     counts;
	guint        have;
	guint        of;
	const gchar *cause;
	const gchar *reason;
	const gchar *mine;
	gint64       asked_at;
	gint64       changed_at;
} AxisPortalRequestView;

/* One kind's quota: "limit", "left" and "window_ends" (seconds since 1970). */
typedef struct
{
	guint  limit;
	guint  left;
	gint64 window_ends;
} AxisPortalAllowance;

/* The writer: a JSON object being built.  Every answer of the API is made
 * with these, so every string in one is escaped by axis_portal_writer. */
typedef struct _AxisPortalWriter AxisPortalWriter;

/* Begins the answer's object. */
AxisPortalWriter *
axis_portal_writer_new(void);

/* Ends the object.  Returns: (transfer full): the JSON; frees @writer. */
gchar *
axis_portal_writer_finish(AxisPortalWriter *writer);

void
axis_portal_write_string(AxisPortalWriter *writer, const gchar *name, const gchar *value);

void
axis_portal_write_int(AxisPortalWriter *writer, const gchar *name, gint64 value);

void
axis_portal_write_boolean(AxisPortalWriter *writer, const gchar *name, gboolean value);

/* "@name": [ "a", "b" ] of a %NULL-ended list. */
void
axis_portal_write_strings(AxisPortalWriter *writer, const gchar *name, const gchar *const *values);

/* "@name": { the title's members } */
void
axis_portal_write_title(AxisPortalWriter *writer, const gchar *name, const AxisPortalTitle *title);

/* "@name": [ titles ] */
void
axis_portal_write_titles(AxisPortalWriter *writer, const gchar *name, const AxisPortalTitle *titles, guint n);

void
axis_portal_write_request(AxisPortalWriter *writer, const gchar *name, const AxisPortalRequestView *request);

void
axis_portal_write_requests(AxisPortalWriter *writer, const gchar *name, const AxisPortalRequestView *requests,
                           guint n);

/* "@name": { "limit", "left", "window_ends" } */
void
axis_portal_write_allowance(AxisPortalWriter *writer, const gchar *name, const AxisPortalAllowance *allowance);

/* "@name": [ { "season": n, "why": "available" | "requested" } ... ]: the
 * seasons a request left out, and why. */
void
axis_portal_write_left_out(AxisPortalWriter *writer, const gchar *name, const gint *available, guint n_available,
                           const gint *requested, guint n_requested);

G_END_DECLS

#endif /* AXIS_PORTAL_PROTO_H */
