/*
 * axis-test-download.h - test support: the "fixture" download client kind
 * and the fake download client it talks to
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A kind for tests only.  It is not part of the daemon (it lives here,
 * among the tests' support code), so it is never registered in a normal
 * build.  It speaks a small JSON API, all POST, to a fake client on
 * loopback (axis-test-plex.h with a scripted handler), the way a real kind
 * speaks its client's:
 *
 *   POST /api/login   {"password"}                   -> a session cookie
 *   POST /api/info    {}                             -> {"version","categories"}
 *   POST /api/add     {"name","category","priority",
 *                      "magnet" | "file","file_name"} -> {"id"}
 *   POST /api/list    {"category"}                   -> {"items":[...]}
 *   POST /api/remove  {"id","delete_data"}           -> {}
 *   POST /api/folder  {"category"}                   -> {"dir"} (both kinds)
 *
 * Every call but the login needs the session cookie; an expired session
 * is answered 401 and the kind logs in again once.  The fake's list
 * ignores the category it is given and answers with everything it holds,
 * so that the layer's own filtering is what keeps other programs'
 * downloads out.
 */

#ifndef AXIS_TEST_DOWNLOAD_H
#define AXIS_TEST_DOWNLOAD_H

#include <glib.h>

#include "axis-test-plex.h"
#include "download/axis-download-kind.h"

G_BEGIN_DECLS

#define AXIS_TEST_DOWNLOAD_PASSWORD "fixture-password-d41c9e07b3"

/* The torrent kind ("fixture") and the Usenet kind ("fixturenzb"). */
const AxisDownloadKindInfo *
axis_test_download_kind(AxisDownloadProtocol protocol);

typedef struct _AxisTestDownloadFake AxisTestDownloadFake;

/**
 * AxisTestDownloadMode:
 * @AXIS_TEST_DOWNLOAD_NORMAL: answers
 * @AXIS_TEST_DOWNLOAD_DOWN: answers nothing at all (the head never comes)
 * @AXIS_TEST_DOWNLOAD_ADD_HANGS: takes an added release, then never answers
 * @AXIS_TEST_DOWNLOAD_ADD_FAILS: takes an added release, then answers 500
 * @AXIS_TEST_DOWNLOAD_ADD_REFUSED: refuses an added release (400)
 * @AXIS_TEST_DOWNLOAD_REDIRECT: answers every request with a redirect to
 *   the location set with axis_test_download_fake_set_redirect()
 * @AXIS_TEST_DOWNLOAD_LIST_FAILS: answers everything but the list, which
 *   it answers with 500
 */
typedef enum
{
	AXIS_TEST_DOWNLOAD_NORMAL,
	AXIS_TEST_DOWNLOAD_DOWN,
	AXIS_TEST_DOWNLOAD_ADD_HANGS,
	AXIS_TEST_DOWNLOAD_ADD_FAILS,
	AXIS_TEST_DOWNLOAD_ADD_REFUSED,
	AXIS_TEST_DOWNLOAD_REDIRECT,
	AXIS_TEST_DOWNLOAD_LIST_FAILS
} AxisTestDownloadMode;

/* A fake client on 127.0.0.1 that takes @password. */
AxisTestDownloadFake *
axis_test_download_fake_new(const gchar *password);

void
axis_test_download_fake_free(AxisTestDownloadFake *self);

/* "http://127.0.0.1:PORT" */
gchar *
axis_test_download_fake_base(AxisTestDownloadFake *self);

/* The server underneath, for what it received. */
AxisTestPlex *
axis_test_download_fake_server(AxisTestDownloadFake *self);

void
axis_test_download_fake_set_mode(
	AxisTestDownloadFake *self,
	AxisTestDownloadMode  mode
);

void
axis_test_download_fake_set_redirect(
	AxisTestDownloadFake *self,
	const gchar          *location
);

/* Where, on a real disk, the client's downloads are: a removal that asks for
 * the data deleted removes @dir/<item name> (the client's own names of the
 * files, as a real client does; any other name of a file keeps it).  Without
 * this nothing is deleted on disk.  "remove-data" counts such removals. */
void
axis_test_download_fake_set_data_dir(
	AxisTestDownloadFake *self,
	const gchar          *dir
);

/* As SABnzbd does (seen from 5.1.3): a removal that asks for the data deleted
 * deletes the files of a failed item only; a completed item's files stay
 * where they are ("remove-kept-files" counts such removals).  Off by
 * default. */
void
axis_test_download_fake_keep_completed(
	AxisTestDownloadFake *self,
	gboolean              keep
);

/* What either kind's category_folder() is told: the client's folder for
 * Axis's category ("folder" counts the calls).  %NULL (the default): the
 * client says the category has none (404). */
void
axis_test_download_fake_set_category_dir(
	AxisTestDownloadFake *self,
	const gchar          *dir
);

/* Item @id is listed as an entry the fixture kind reads by its id only, @why
 * being what it says of it (as a real kind does an entry with no readable
 * name); %NULL: readable again. */
void
axis_test_download_fake_set_unreadable(
	AxisTestDownloadFake *self,
	const gchar          *id,
	const gchar          *why
);

/* A removal of an item the fake does not have is answered 410, which the
 * kinds read as %AXIS_DOWNLOAD_ERROR_GONE (as Deluge's "not in session"
 * is); off by default (404, a refusal). */
void
axis_test_download_fake_say_gone(
	AxisTestDownloadFake *self,
	gboolean              gone
);

/* Ends the current session: the next call is answered 401. */
void
axis_test_download_fake_expire_session(AxisTestDownloadFake *self);

/* The session cookie's value handed out last (for checking where it went). */
gchar *
axis_test_download_fake_session(AxisTestDownloadFake *self);

/**
 * axis_test_download_fake_put:
 * @state: "queued", "downloading", "paused", "completed" or "failed"
 * @hash: (nullable)
 * @path: (nullable)
 * @message: (nullable)
 *
 * Adds or changes an item the client holds (as if a person, or the client
 * itself, did it).
 */
void
axis_test_download_fake_put(
	AxisTestDownloadFake *self,
	const gchar          *id,
	const gchar          *name,
	const gchar          *category,
	const gchar          *hash,
	const gchar          *state,
	gint64                size,
	gint64                done,
	const gchar          *path,
	const gchar          *message
);

/* Changes an item's state, how much is done, its path and message. */
void
axis_test_download_fake_set(
	AxisTestDownloadFake *self,
	const gchar          *id,
	const gchar          *state,
	gint64                done,
	const gchar          *path,
	const gchar          *message
);

/* The item vanishes (removed by someone else). */
void
axis_test_download_fake_drop(
	AxisTestDownloadFake *self,
	const gchar          *id
);

gboolean
axis_test_download_fake_has(
	AxisTestDownloadFake *self,
	const gchar          *id
);

/* How many items it holds. */
guint
axis_test_download_fake_n_items(AxisTestDownloadFake *self);

/* The identifier the fake gave the last release it took (or NULL). */
gchar *
axis_test_download_fake_last_id(AxisTestDownloadFake *self);

/* How many calls of @op ("login", "info", "add", "list", "remove") it was
 * sent, and how many of those named @id (for "remove": the id removed;
 * NULL counts all). */
guint
axis_test_download_fake_count(
	AxisTestDownloadFake *self,
	const gchar          *op,
	const gchar          *id
);

/* What the last add carried: its category and priority. */
gchar *
axis_test_download_fake_last_add_field(
	AxisTestDownloadFake *self,
	const gchar          *field
);

/* A small valid .torrent of @name (its info-hash in @hash when wanted). */
GBytes *
axis_test_download_torrent(
	const gchar  *name,
	gchar       **hash
);

G_END_DECLS

#endif /* AXIS_TEST_DOWNLOAD_H */
