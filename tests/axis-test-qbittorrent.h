/*
 * axis-test-qbittorrent.h - test support: a stand-in for qBittorrent's Web
 * API v2, on loopback, in the 4.x or the 5.x way
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * THE STAND-IN IS HAND-WRITTEN.  No qBittorrent was available: it answers as
 * qBittorrent's published Web API and the author's memory of 4.6 and 5.0 say
 * it does, not as a recording does.  What is believed and not verified is
 * listed in tests/fixtures/qbittorrent/README.md.
 *
 * It serves, under an optional path prefix, /api/v2/auth/login,
 * /app/version, /app/webapiVersion, /torrents/categories,
 * /torrents/createCategory, /torrents/add (multipart), /torrents/info and
 * /torrents/delete, in the generation's way:
 *
 *   4.x  a login is answered 200 "Ok." or 200 "Fails."; the cookie is SID;
 *        a paused torrent is pausedDL / pausedUP
 *   5.x  a login is answered 204 or 401; the cookie is QBT_SID_<port>; a
 *        stopped torrent is stoppedDL / stoppedUP
 *
 * Both: every call but the login needs the session cookie and is answered 403
 * without it; a request whose Referer or Origin is not the server's own
 * address is answered 401 (its CSRF protection); an address that failed to
 * log in too many times is banned, and then every request is answered 403.
 * It records every request, so a test can say what was asked, in what order,
 * and with which headers.
 */

#ifndef AXIS_TEST_QBITTORRENT_H
#define AXIS_TEST_QBITTORRENT_H

#include <glib.h>

#include "axis-test-plex.h"

G_BEGIN_DECLS

typedef enum
{
	AXIS_TEST_QBIT_V4,
	AXIS_TEST_QBIT_V5
} AxisTestQbitGeneration;

typedef enum
{
	AXIS_TEST_QBIT_NORMAL,
	AXIS_TEST_QBIT_DOWN,		/* the head never comes */
	AXIS_TEST_QBIT_REDIRECT		/* every request is answered with a redirect */
} AxisTestQbitMode;

typedef enum
{
	AXIS_TEST_QBIT_ADD_FAILS,	/* /torrents/add answers "Fails." and adds nothing */
	AXIS_TEST_QBIT_ADD_HIDDEN,	/* answers "Ok." and never lists the torrent */
	AXIS_TEST_QBIT_ADD_HANGS,	/* takes the torrent and never answers */
	AXIS_TEST_QBIT_ADD_MERGES,	/* a torrent it has already is answered "Ok." (and left in its category) */
	AXIS_TEST_QBIT_NO_COOKIE,	/* a login is accepted and sets no cookie */
	AXIS_TEST_QBIT_CATEGORY_409,	/* createCategory answers 409 and makes nothing */
	AXIS_TEST_QBIT_NO_CONTENT_PATH,	/* the list has no content_path (qBittorrent before 4.3.2) */
	AXIS_TEST_QBIT_DELETE_KEEPS,	/* delete answers 200 and removes nothing */
	AXIS_TEST_QBIT_SUBCATEGORIES,	/* the category filter of a list takes sub-categories along */
	AXIS_TEST_QBIT_ADD_RECATEGORISES, /* an added torrent is put in the category "manual" (a rule of the person's) */
	AXIS_TEST_QBIT_N_FLAGS
} AxisTestQbitFlag;

typedef struct _AxisTestQbit AxisTestQbit;

AxisTestQbit *
axis_test_qbit_new(
	AxisTestQbitGeneration  generation,
	const gchar            *username,
	const gchar            *password
);

void
axis_test_qbit_free(AxisTestQbit *self);

/* "http://127.0.0.1:PORT" */
gchar *
axis_test_qbit_base(AxisTestQbit *self);

AxisTestPlex *
axis_test_qbit_server(AxisTestQbit *self);

/* Serves under @prefix ("/qb"), which the client gives as its base path. */
void
axis_test_qbit_set_prefix(
	AxisTestQbit *self,
	const gchar  *prefix
);

void
axis_test_qbit_set_mode(
	AxisTestQbit     *self,
	AxisTestQbitMode  mode
);

/* Where a redirect (the mode %AXIS_TEST_QBIT_REDIRECT) points. */
void
axis_test_qbit_set_redirect(
	AxisTestQbit *self,
	const gchar  *location
);

void
axis_test_qbit_set_flag(
	AxisTestQbit     *self,
	AxisTestQbitFlag  flag,
	gboolean          on
);

/* After this many wrong logins the address is banned (0: never; the default
 * is 5, as qBittorrent's). */
void
axis_test_qbit_set_ban_after(
	AxisTestQbit *self,
	guint         failures
);

/* The address is banned now, or no longer. */
void
axis_test_qbit_set_banned(
	AxisTestQbit *self,
	gboolean      banned
);

/* Every answer to the path @path (after /api/v2, "/torrents/info") is this,
 * until it is cleared. */
void
axis_test_qbit_set_raw(
	AxisTestQbit *self,
	const gchar  *path,
	guint         status,
	const gchar  *content_type,
	const gchar  *body
);

void
axis_test_qbit_clear_raw(
	AxisTestQbit *self,
	const gchar  *path
);

/* The session ends now: the next call is answered 403. */
void
axis_test_qbit_expire_session(AxisTestQbit *self);

/* The session ends just before the next request for @path ("/torrents/add"). */
void
axis_test_qbit_expire_before(
	AxisTestQbit *self,
	const gchar  *path
);

/* ── What it holds ──────────────────────────────────────────────── */

/* A category a person made. */
void
axis_test_qbit_add_category(
	AxisTestQbit *self,
	const gchar  *name
);

gboolean
axis_test_qbit_has_category(
	AxisTestQbit *self,
	const gchar  *name
);

/* Forgets every category. */
void
axis_test_qbit_clear_categories(AxisTestQbit *self);

/* A category's save path, as /torrents/categories lists it ("" or %NULL:
 * none); the category is made when it is not there. */
void
axis_test_qbit_set_category_path(
	AxisTestQbit *self,
	const gchar  *name,
	const gchar  *path
);

/* What /app/defaultSavePath answers ("/downloads" by default). */
void
axis_test_qbit_set_default_path(
	AxisTestQbit *self,
	const gchar  *path
);

/**
 * axis_test_qbit_put:
 * @category: (nullable): none when %NULL
 * @state: a state word, as qBittorrent says it
 * @progress: 0 to 1
 * @content_path: (nullable)
 *
 * Adds or changes a torrent the stand-in holds (as a person, or qBittorrent
 * itself, would).
 */
void
axis_test_qbit_put(
	AxisTestQbit *self,
	const gchar  *hash,
	const gchar  *name,
	const gchar  *category,
	const gchar  *state,
	gint64        size,
	gdouble       progress,
	const gchar  *save_path,
	const gchar  *content_path
);

void
axis_test_qbit_set_state(
	AxisTestQbit *self,
	const gchar  *hash,
	const gchar  *state,
	gdouble       progress
);

void
axis_test_qbit_drop(
	AxisTestQbit *self,
	const gchar  *hash
);

gboolean
axis_test_qbit_has(
	AxisTestQbit *self,
	const gchar  *hash
);

/* The torrent's category, or %NULL (none, or no such torrent). */
gchar *
axis_test_qbit_category_of(
	AxisTestQbit *self,
	const gchar  *hash
);

guint
axis_test_qbit_n_torrents(AxisTestQbit *self);

/* ── What it received ───────────────────────────────────────────── */

/* How many calls begin with @prefix.  The calls are noted as "login",
 * "version", "webapi", "categories", "create-category:NAME", "add",
 * "info:category=NAME" or "info:hashes=HASH", "delete:HASH:data" or
 * "delete:HASH:keep", "unauthorised:PATH", "banned:PATH". */
guint
axis_test_qbit_count(
	AxisTestQbit *self,
	const gchar  *prefix
);

/* The calls in order, as one text. */
gchar *
axis_test_qbit_calls(AxisTestQbit *self);

/* How many requests had a Referer or an Origin that was not its own address,
 * or a method or path it does not serve. */
guint
axis_test_qbit_bad_requests(AxisTestQbit *self);

/* How many logins it answered while the address was banned. */
guint
axis_test_qbit_banned_logins(AxisTestQbit *self);

/* How many wrong logins it counted. */
guint
axis_test_qbit_failed_logins(AxisTestQbit *self);

/* The form of the last add: "urls", "torrents-name" (the uploaded file's name),
 * "torrents-size", "category", "paused", "stopped"; %NULL when it had none. */
gchar *
axis_test_qbit_last_add(
	AxisTestQbit *self,
	const gchar  *field
);

/* The last login's user name. */
gchar *
axis_test_qbit_last_login_user(AxisTestQbit *self);

/* Every session cookie value ever given (a GPtrArray of strings the caller
 * frees). */
GPtrArray *
axis_test_qbit_sessions(AxisTestQbit *self);

/* The name of the cookie it sets. */
gchar *
axis_test_qbit_cookie_name(AxisTestQbit *self);

G_END_DECLS

#endif /* AXIS_TEST_QBITTORRENT_H */
