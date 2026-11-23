/*
 * test-web-storage.c - the Volumes and Roots pages
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Each test makes its own database, migrates it and runs the real shell,
 * a real hub and the real storage module on an ephemeral loopback port,
 * with sessions opened in an auth without a database.  Disks are
 * directories under a scratch directory, made offline by moving them
 * away, as in test-storage.c.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <fcntl.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <htmx-glib.h>
#include <libsoup/soup.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "axis-test-db.h"
#include "axis-test-jobs.h"
#include "axis-test-storage.h"
#include "axis-test-web.h"
#include "core/axis-module.h"
#include "core/axis-module-state.h"
#include "core/axis-settings.h"
#include "core/axis-size.h"
#include "jobs/axis-job-queue.h"
#include "jobs/axis-job-registry.h"
#include "storage/axis-storage.h"
#include "storage/axis-volume-marker.h"
#include "web/axis-session-store.h"
#include "web/axis-web.h"
#include "web/axis-web-auth.h"
#include "web/axis-web-hub.h"
#include "web/axis-web-settings.h"
#include "web/axis-web-storage.h"

#define MARKUP_NAME "<script>alert(1)</script> & \"q\""

typedef struct
{
	AxisTestDb         *db;
	AxisDbPool         *pool;
	AxisSettings       *settings;
	AxisWebSettings    *web_settings;
	AxisStorage        *storage;
	AxisWebStorage     *storage_ui;
	AxisModuleRegistry *registry;
	HtmxServer         *server;
	AxisWebAuth        *auth;
	AxisWeb            *web;
	AxisWebHub         *hub;
	SoupSession        *session;
	guint16             port;
	gchar              *admin;
	gchar              *viewer;
	gchar              *dir;
	gchar              *media;
	gchar              *stash;
} Fixture;

#define SKIP_WITHOUT_DB(fx) do { if ((fx)->db == NULL) return; } while (0)

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;

	memset(fx, 0, sizeof(*fx));
	axis_test_log_init();
	axis_test_log_reset();
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*is offline at*");
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*does not match what is at*");
	axis_session_store_set_idle_seconds(30 * 24 * 3600);

	fx->dir = axis_test_storage_dir_new();
	fx->media = g_build_filename(fx->dir, "media", NULL);
	fx->stash = g_build_filename(fx->dir, "stash", NULL);
	axis_test_storage_mkdir(fx->media);
	axis_test_storage_mkdir(fx->stash);

	fx->db = axis_test_db_new();
	if (fx->db == NULL)
		return;
	fx->pool = axis_test_jobs_pool(fx->db, 8);

	fx->settings = axis_settings_new();
	fx->web_settings = axis_web_settings_new(fx->settings, fx->pool);
	fx->hub = axis_web_hub_new(NULL);
	fx->storage = axis_storage_new(fx->pool, fx->settings);
	fx->storage_ui = axis_web_storage_new(fx->storage, fx->hub);

	fx->registry = axis_module_registry_new();
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_settings_get_module(),
		fx->web_settings, &error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_storage_get_module(),
		fx->storage_ui, &error));
	g_assert_no_error(error);
	g_assert_true(axis_settings_add_modules(fx->settings, fx->registry, &error));
	g_assert_true(axis_settings_load(fx->settings, fx->pool, &error));
	g_assert_true(axis_module_state_load(fx->registry, fx->pool, &error));
	g_assert_true(axis_storage_load(fx->storage, &error));
	g_assert_no_error(error);
	axis_web_settings_sync(fx->web_settings);

	fx->server = htmx_server_new();
	g_assert_true(htmx_server_listen_on(fx->server, "127.0.0.1", 0, &error));
	fx->auth = axis_web_auth_new(NULL, "", AXIS_SECURE_COOKIES_AUTO, &error);
	g_assert_no_error(error);
	fx->web = axis_web_new(fx->server, fx->registry, fx->auth);
	axis_web_hub_install(fx->hub, fx->web, fx->auth);
	g_assert_true(htmx_server_start(fx->server, &error));
	g_assert_true(axis_module_registry_start_all(fx->registry, &error));
	g_assert_no_error(error);

	fx->admin = axis_web_auth_open_test_session(fx->auth, "root", AXIS_USER_ROLE_ADMIN);
	fx->viewer = axis_web_auth_open_test_session(fx->auth, "viewer", AXIS_USER_ROLE_USER);
	fx->port = axis_test_web_port(fx->server);
	fx->session = soup_session_new_with_options("timeout", 20, "idle-timeout", 20, NULL);
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	axis_session_store_set_idle_seconds(30 * 24 * 3600);
	if (fx->db == NULL) {
		axis_test_storage_dir_free(fx->dir);
		g_free(fx->media);
		g_free(fx->stash);
		axis_test_log_reset();
		return;
	}

	g_object_unref(fx->session);
	htmx_server_stop(fx->server);
	axis_web_storage_stop(fx->storage_ui);
	axis_web_settings_stop(fx->web_settings);
	while (g_main_context_iteration(NULL, FALSE))
		;
	axis_web_free(fx->web);
	axis_web_hub_free(fx->hub);
	axis_web_storage_free(fx->storage_ui);
	axis_web_settings_free(fx->web_settings);
	axis_web_auth_free(fx->auth);
	g_object_unref(fx->server);
	axis_module_registry_free(fx->registry);
	axis_storage_stop(fx->storage);
	axis_storage_unref(fx->storage);
	axis_settings_unref(fx->settings);
	axis_db_pool_free(fx->pool);
	axis_test_db_free(fx->db);
	axis_test_storage_dir_free(fx->dir);
	g_free(fx->media);
	g_free(fx->stash);
	g_free(fx->admin);
	g_free(fx->viewer);
	axis_test_log_reset();
}

/* ── Helpers ────────────────────────────────────────────────────── */

static AxisTestReply
get_as(
	Fixture     *fx,
	const gchar *cookie,
	const gchar *path
){
	return axis_test_web_request(fx->session, fx->port, "GET", path, cookie, NULL, NULL);
}

static gchar *
csrf_for(
	Fixture     *fx,
	const gchar *cookie
){
	AxisTestReply page = get_as(fx, cookie, "/storage");
	gchar *token = axis_test_web_csrf_token(page.body);

	g_assert_nonnull(token);
	axis_test_reply_clear(&page);
	return token;
}

static gchar *
form_of(const gchar *first, ...)
{
	GString *form = g_string_new(NULL);
	const gchar *name = first;
	va_list ap;

	va_start(ap, first);
	while (name != NULL) {
		const gchar *value = va_arg(ap, const gchar *);
		g_autofree gchar *escaped = g_uri_escape_string(value, NULL, FALSE);

		g_string_append_printf(form, "%s%s=%s", form->len > 0 ? "&" : "", name, escaped);
		name = va_arg(ap, const gchar *);
	}
	va_end(ap);

	return g_string_free(form, FALSE);
}

static AxisTestReply
post_as(
	Fixture     *fx,
	const gchar *cookie,
	const gchar *path,
	const gchar *csrf,
	const gchar *form
){
	const gchar *extra[4];
	guint n = 0;

	if (csrf != NULL) {
		extra[n++] = "X-CSRF-Token";
		extra[n++] = csrf;
	}
	extra[n] = NULL;

	return axis_test_web_request(fx->session, fx->port, "POST", path, cookie, extra, form);
}

static void
assert_contains(
	const gchar *haystack,
	const gchar *needle
){
	g_autofree gchar *plain = haystack != NULL ? axis_test_web_unbroken(haystack) : NULL;

	if (plain == NULL || strstr(plain, needle) == NULL)
		g_error("expected to find '%s' in:\n%s", needle, haystack != NULL ? haystack : "(null)");
}

static void
assert_lacks(
	const gchar *haystack,
	const gchar *needle
){
	if (haystack != NULL && strstr(haystack, needle) != NULL)
		g_error("did not expect '%s' in:\n%s", needle, haystack);
}

static void
assert_no_inline_code(const gchar *html)
{
	assert_lacks(html, "style=\"");
	assert_lacks(html, "<style");
	assert_lacks(html, "<script>");
	assert_lacks(html, "onclick");
	assert_lacks(html, "javascript:");
}

static gchar *
make_disk(
	Fixture     *fx,
	const gchar *name
){
	gchar *path = g_build_filename(fx->media, name, NULL);
	g_autofree gchar *file = g_build_filename(path, "readme.txt", NULL);

	axis_test_storage_mkdir(path);
	axis_test_storage_write(file, "media\n");
	return path;
}

static gint64
add_volume(
	Fixture     *fx,
	const gchar *path,
	const gchar *name
){
	g_autoptr(AxisVolumeInspection) inspection = NULL;
	g_autoptr(GError) error = NULL;
	AxisVolumeAddRequest request;
	gint64 id = 0;

	g_assert_true(axis_storage_inspect_volume(fx->storage, path, &inspection, &error));
	g_assert_cmpint(inspection->plan, !=, AXIS_VOLUME_PLAN_REFUSE);
	memset(&request, 0, sizeof(request));
	request.path = inspection->path;
	request.name = name;
	request.plan = inspection->plan;
	request.marker_uuid = inspection->marker_uuid;
	request.dev = inspection->dev;
	request.ino = inspection->ino;
	request.fsid = inspection->fsid;
	request.total_bytes = inspection->total_bytes;
	request.not_mount_point_ok = TRUE;
	g_assert_true(axis_storage_add_volume(fx->storage, &request, &id, &error));
	g_assert_no_error(error);
	return id;
}

/* Storage writable (it is read-only by default). */
static void
set_writable(Fixture *fx)
{
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_settings_set(fx->settings, AXIS_STORAGE_SETTING_READ_ONLY, "false", &error));
	g_assert_no_error(error);
	while (g_main_context_iteration(NULL, FALSE))
		;
}

/* A further volume.  These disks are directories of one filesystem, which
 * Axis refuses as two volumes: the volumes added before are first made to
 * say nothing of their filesystem, as disks last seen elsewhere (as
 * axis_test_library_add_volume_beside() does). */
static gint64
add_volume_beside(
	Fixture     *fx,
	const gchar *path,
	const gchar *name
){
	axis_test_db_exec(fx->db, "UPDATE volume SET fsid = '', device = ''");
	return add_volume(fx, path, name);
}

static void
unmount_disk(
	Fixture     *fx,
	const gchar *path,
	const gchar *as
){
	g_autofree gchar *away = g_build_filename(fx->stash, as, NULL);

	g_assert_cmpint(g_rename(path, away), ==, 0);
	axis_test_storage_mkdir(path);
}

/* Puts back the disk unmount_disk() took away as @as. */
static void
mount_disk(
	Fixture     *fx,
	const gchar *path,
	const gchar *as
){
	g_autofree gchar *away = g_build_filename(fx->stash, as, NULL);

	g_assert_cmpint(g_rmdir(path), ==, 0);
	g_assert_cmpint(g_rename(away, path), ==, 0);
}

static void
check_now(
	Fixture *fx,
	gint64   id
){
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_storage_check_volume(fx->storage, id, NULL, &error));
	g_assert_no_error(error);
}

/* The confirmation form for adding @path, as the page would post it. */
static gchar *
confirm_form(
	const gchar *path,
	const gchar *name,
	gboolean     not_mount_point_ok
){
	struct stat st;
	struct statfs fs;
	struct statvfs vfs;
	g_autofree gchar *dev = NULL;
	g_autofree gchar *ino = NULL;
	g_autofree gchar *fsid = NULL;
	g_autofree gchar *size = NULL;

	g_assert_cmpint(g_stat(path, &st), ==, 0);
	g_assert_cmpint(statfs(path, &fs), ==, 0);
	g_assert_cmpint(statvfs(path, &vfs), ==, 0);
	dev = g_strdup_printf("%" G_GUINT64_FORMAT, (guint64)st.st_dev);
	ino = g_strdup_printf("%" G_GUINT64_FORMAT, (guint64)st.st_ino);
	fsid = g_strdup_printf("%08x%08x", (guint)fs.f_fsid.__val[0], (guint)fs.f_fsid.__val[1]);
	size = g_strdup_printf("%" G_GUINT64_FORMAT, (guint64)vfs.f_blocks * (guint64)vfs.f_frsize);
	if (not_mount_point_ok)
		return form_of("path", path, "name", name, "plan", "write", "dev", dev, "ino", ino,
			"fsid", fsid, "size", size, "confirm", "yes", "not_mount_point_ok", "yes", NULL);
	return form_of("path", path, "name", name, "plan", "write", "dev", dev, "ino", ino,
		"fsid", fsid, "size", size, "confirm", "yes", NULL);
}

static guint
log_mentions(const gchar *text)
{
	g_autofree gchar *pattern = g_strdup_printf("*%s*", text);
	static const GLogLevelFlags levels[] = {
		G_LOG_LEVEL_DEBUG, G_LOG_LEVEL_INFO, G_LOG_LEVEL_MESSAGE, G_LOG_LEVEL_WARNING,
		G_LOG_LEVEL_CRITICAL
	};
	guint count = 0;
	guint i;

	for (i = 0; i < G_N_ELEMENTS(levels); i++)
		count += axis_test_log_count(levels[i], pattern);
	return count;
}

/* ── The list ───────────────────────────────────────────────────── */

/* Spec: Free space is shown */
static void
test_volumes_page(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *usb = NULL;
	g_autofree gchar *main_disk = NULL;
	AxisTestReply viewer;
	AxisTestReply admin;

	SKIP_WITHOUT_DB(fx);
	/* One volume that went away, one that is there. */
	usb = make_disk(fx, "usb");
	add_volume(fx, usb, "USB");
	unmount_disk(fx, usb, "usb");
	check_now(fx, 1);
	main_disk = make_disk(fx, "main");
	add_volume_beside(fx, main_disk, "Main");

	viewer = get_as(fx, fx->viewer, "/storage");
	g_assert_cmpuint(viewer.status, ==, 200);
	assert_contains(viewer.body, ">Main<");
	assert_contains(viewer.body, ">USB<");
	assert_contains(viewer.body, main_disk);
	assert_contains(viewer.body, "state-online");
	assert_contains(viewer.body, "state-offline");
	/* The online one: its total and free space as of its last check. */
	assert_contains(viewer.body, "free of");
	assert_contains(viewer.body, "available to Axis");
	/* The offline one: when it was last seen, and its last known space. */
	assert_contains(viewer.body, "Last seen online");
	assert_contains(viewer.body, "Last known:");
	assert_contains(viewer.body, "the disk is not mounted");
	/* Live: the page follows the storage topic. */
	assert_contains(viewer.body, "topic=storage");
	assert_contains(viewer.body, "id=\"volume-1-state\"");
	assert_contains(viewer.body, "Read-only storage is on");
	/* Looking is for everyone; adding is for administrators. */
	assert_lacks(viewer.body, "/storage/volumes/inspect");
	assert_no_inline_code(viewer.body);

	admin = get_as(fx, fx->admin, "/storage");
	g_assert_cmpuint(admin.status, ==, 200);
	assert_contains(admin.body, "action=\"/storage/volumes/inspect\"");
	assert_contains(admin.body, "name=\"_csrf\"");
	assert_no_inline_code(admin.body);

	axis_test_reply_clear(&viewer);
	axis_test_reply_clear(&admin);
}

/* The free space of volume @id as the pages put it: "X free of Y". */
static gchar *
free_of(
	Fixture *fx,
	gint64   id
){
	g_autoptr(AxisVolumeInfo) volume = axis_storage_get_volume(fx->storage, id);
	g_autofree gchar *free_text = NULL;
	g_autofree gchar *total = NULL;

	g_assert_nonnull(volume);
	g_assert_true(volume->has_space);
	free_text = axis_size_format(volume->free_bytes);
	total = axis_size_format(volume->total_bytes);
	return g_strdup_printf("%s free of %s", free_text, total);
}

/* Spec (web-shell): Space; Figures agree; Nothing needs attention;
 * Something needs attention (a volume offline). */
static void
test_dashboard_space(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *usb = NULL;
	g_autofree gchar *main_disk = NULL;
	g_autofree gchar *main_free = NULL;
	g_autofree gchar *usb_free = NULL;
	g_autofree gchar *usb_last = NULL;
	AxisTestReply board;
	AxisTestReply storage;
	gint64 main_id;
	gint64 usb_id;

	SKIP_WITHOUT_DB(fx);
	/* Two "disks" are directories of one filesystem, which storage
	 * refuses as two volumes (add_volume_beside()); the first is away while
	 * the second is added. */
	main_disk = make_disk(fx, "main");
	main_id = add_volume(fx, main_disk, "Main");
	unmount_disk(fx, main_disk, "main");
	check_now(fx, main_id);
	usb = make_disk(fx, "usb");
	usb_id = add_volume_beside(fx, usb, "USB");
	mount_disk(fx, main_disk, "main");
	check_now(fx, main_id);
	check_now(fx, usb_id);

	/* Both there: each with its space and a bar, nothing to attend to. */
	board = get_as(fx, fx->viewer, "/");
	g_assert_cmpuint(board.status, ==, 200);
	main_free = free_of(fx, main_id);
	assert_contains(board.body, "id=\"dash-space\"");
	assert_contains(board.body, "<a href=\"/storage\">Disk space</a>");
	assert_contains(board.body, "<bdi>Main</bdi>");
	assert_contains(board.body, "<bdi>USB</bdi>");
	assert_contains(board.body, main_free);
	assert_contains(board.body, "class=\"space-bar\"");
	assert_contains(board.body, "% used\"");
	assert_contains(board.body, "Space checked ");
	/* The card of what needs attention is an administrator's. */
	assert_lacks(board.body, "dash-attention");
	assert_lacks(board.body, "state-offline");
	assert_no_inline_code(board.body);
	/* The page behind it says the same. */
	storage = get_as(fx, fx->viewer, "/storage");
	assert_contains(storage.body, main_free);
	axis_test_reply_clear(&board);
	axis_test_reply_clear(&storage);

	/* One goes away: called out at once on the Disk space card, with its
	 * last known space.  (Under what needs attention it is listed after its
	 * wait, to administrators: add-attention-notices, tests/test-web-attention.c.) */
	usb_free = free_of(fx, usb_id);
	unmount_disk(fx, usb, "usb");
	check_now(fx, usb_id);
	board = get_as(fx, fx->viewer, "/");
	usb_last = g_strdup_printf("Last known: %s", usb_free);
	assert_contains(board.body, "state-offline");
	assert_contains(board.body, usb_last);
	assert_lacks(board.body, "dash-attention");
	storage = get_as(fx, fx->viewer, "/storage");
	assert_contains(storage.body, usb_last);
	axis_test_reply_clear(&board);
	axis_test_reply_clear(&storage);
}

static void
test_names_are_escaped(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *disk = NULL;
	AxisTestReply page;
	AxisTestReply detail;

	SKIP_WITHOUT_DB(fx);
	disk = make_disk(fx, "x<b>y");
	add_volume(fx, disk, MARKUP_NAME);

	page = get_as(fx, fx->admin, "/storage");
	assert_contains(page.body, "&lt;script&gt;alert(1)&lt;/script&gt; &amp; &quot;q&quot;");
	assert_contains(page.body, "x&lt;b&gt;y");
	assert_lacks(page.body, "<script>alert");
	assert_lacks(page.body, "x<b>y");
	detail = get_as(fx, fx->admin, "/storage/volumes/1");
	g_assert_cmpuint(detail.status, ==, 200);
	assert_lacks(detail.body, "<script>alert");
	assert_no_inline_code(detail.body);

	axis_test_reply_clear(&page);
	axis_test_reply_clear(&detail);
}

/* ── Adding ─────────────────────────────────────────────────────── */

/* Spec: Adding a volume writes its marker (with the confirmation step) */
static void
test_add_with_confirmation(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *disk = make_disk(fx, "disk1");
	g_autofree gchar *marker = g_build_filename(disk, AXIS_VOLUME_MARKER_NAME, NULL);
	g_autofree gchar *csrf = NULL;
	g_autofree gchar *viewer_csrf = NULL;
	g_autofree gchar *look = NULL;
	g_autofree gchar *confirm = NULL;
	g_autofree gchar *unconfirmed = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	g_autofree gchar *count = NULL;
	AxisTestReply reply;

	SKIP_WITHOUT_DB(fx);
	csrf = csrf_for(fx, fx->admin);
	look = form_of("path", disk, "name", "First disk", NULL);

	/* Looking shows exactly what will be written, and writes nothing. */
	before = axis_test_storage_digest(disk);
	reply = post_as(fx, fx->admin, "/storage/volumes/inspect", csrf, look);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Axis will write one new file");
	assert_contains(reply.body, ".axis-volume");
	assert_contains(reply.body, "Write the marker and add the disk");
	assert_contains(reply.body, "action=\"/storage/volumes\"");
	assert_contains(reply.body, "not a mount point");
	assert_contains(reply.body, "name=\"not_mount_point_ok\"");
	assert_contains(reply.body, "name=\"fsid\"");
	assert_contains(reply.body, "name=\"size\"");
	assert_no_inline_code(reply.body);
	axis_test_reply_clear(&reply);
	after = axis_test_storage_digest(disk);
	g_assert_cmpstr(before, ==, after);

	/* Without the CSRF token, as another user, or without confirming:
	 * nothing. */
	confirm = confirm_form(disk, "First disk", TRUE);
	reply = post_as(fx, fx->admin, "/storage/volumes", NULL, confirm);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	viewer_csrf = csrf_for(fx, fx->viewer);
	reply = post_as(fx, fx->viewer, "/storage/volumes", viewer_csrf, confirm);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->viewer, "/storage/volumes/inspect", viewer_csrf, look);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	unconfirmed = form_of("path", disk, "name", "First disk", "plan", "write", NULL);
	reply = post_as(fx, fx->admin, "/storage/volumes", csrf, unconfirmed);
	g_assert_cmpuint(reply.status, ==, 400);
	axis_test_reply_clear(&reply);
	g_clear_pointer(&unconfirmed, g_free);
	unconfirmed = confirm_form(disk, "First disk", FALSE);
	reply = post_as(fx, fx->admin, "/storage/volumes", csrf, unconfirmed);
	g_assert_cmpuint(reply.status, ==, 409);
	assert_contains(reply.body, "not a mount point");
	axis_test_reply_clear(&reply);
	g_assert_false(g_file_test(marker, G_FILE_TEST_EXISTS));

	/* Confirmed: written, recorded, audited. */
	reply = post_as(fx, fx->admin, "/storage/volumes", csrf, confirm);
	g_assert_cmpuint(reply.status, ==, 303);
	g_assert_cmpstr(axis_test_reply_header(&reply, "Location"), ==, "/storage/volumes/1?done=added");
	axis_test_reply_clear(&reply);
	g_assert_true(g_file_test(marker, G_FILE_TEST_IS_REGULAR));
	count = axis_test_db_query(fx->db, "SELECT count(*) FROM volume WHERE name = 'First disk'");
	g_assert_cmpstr(count, ==, "1");
	g_assert_cmpuint(log_mentions("audit: volume added: id=1 name=First disk"), ==, 1);
	g_assert_cmpuint(log_mentions("by=root"), >=, 1);

	reply = get_as(fx, fx->admin, "/storage/volumes/1?done=added");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "The disk has been added.");
	assert_contains(reply.body, "First disk");
	axis_test_reply_clear(&reply);
}

/* What was confirmed must still be what is there. */
static void
test_add_refuses_changed_directory(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *disk = make_disk(fx, "disk");
	g_autofree gchar *replacement = make_disk(fx, "replacement");
	g_autofree gchar *marker = g_build_filename(disk, AXIS_VOLUME_MARKER_NAME, NULL);
	g_autofree gchar *csrf = NULL;
	g_autofree gchar *confirm = NULL;
	AxisTestReply reply;

	SKIP_WITHOUT_DB(fx);
	csrf = csrf_for(fx, fx->admin);
	confirm = confirm_form(disk, "Disk", TRUE);

	/* Another directory is put at the path after it was shown. */
	unmount_disk(fx, disk, "disk");
	g_assert_cmpint(g_rmdir(disk), ==, 0);
	g_assert_cmpint(g_rename(replacement, disk), ==, 0);
	reply = post_as(fx, fx->admin, "/storage/volumes", csrf, confirm);
	g_assert_cmpuint(reply.status, ==, 409);
	assert_contains(reply.body, "changed since it was shown");
	axis_test_reply_clear(&reply);
	g_assert_false(g_file_test(marker, G_FILE_TEST_EXISTS));
}

static void
test_inspect_refusals(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *usb = g_build_filename(fx->media, "usb", NULL);
	g_autofree gchar *csrf = NULL;
	g_autofree gchar *form = NULL;
	AxisTestReply reply;

	SKIP_WITHOUT_DB(fx);
	csrf = csrf_for(fx, fx->admin);

	/* Malformed paths are refused before anything is looked at, and the
	 * reason does not repeat them. */
	form = form_of("path", "relative/path", NULL);
	reply = post_as(fx, fx->admin, "/storage/volumes/inspect", csrf, form);
	g_assert_cmpuint(reply.status, ==, 400);
	assert_contains(reply.body, "must be absolute");
	axis_test_reply_clear(&reply);
	g_clear_pointer(&form, g_free);
	form = form_of("path", "/media/a\nforged log line", NULL);
	reply = post_as(fx, fx->admin, "/storage/volumes/inspect", csrf, form);
	g_assert_cmpuint(reply.status, ==, 400);
	assert_contains(reply.body, "control characters");
	axis_test_reply_clear(&reply);
	g_assert_cmpuint(log_mentions("forged log line"), ==, 0);
	g_clear_pointer(&form, g_free);
	form = form_of("path", "/media/../etc", NULL);
	reply = post_as(fx, fx->admin, "/storage/volumes/inspect", csrf, form);
	g_assert_cmpuint(reply.status, ==, 400);
	axis_test_reply_clear(&reply);

	/* An empty directory that is not a mount point: an unmounted disk. */
	axis_test_storage_mkdir(usb);
	g_clear_pointer(&form, g_free);
	form = form_of("path", usb, NULL);
	reply = post_as(fx, fx->admin, "/storage/volumes/inspect", csrf, form);
	g_assert_cmpuint(reply.status, ==, 409);
	assert_contains(reply.body, "Mount the disk first");
	assert_lacks(reply.body, "action=\"/storage/volumes\"");
	axis_test_reply_clear(&reply);
}

/* ── One volume, roots ──────────────────────────────────────────── */

/* Spec: Add a root; Remove a root (through the pages) */
static void
test_roots_pages(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *disk = make_disk(fx, "disk");
	g_autofree gchar *movies = g_build_filename(disk, "Movies", NULL);
	g_autofree gchar *film = g_build_filename(movies, "film.mkv", NULL);
	g_autofree gchar *csrf = NULL;
	g_autofree gchar *form = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	g_autofree gchar *count = NULL;
	AxisTestReply reply;

	SKIP_WITHOUT_DB(fx);
	axis_test_storage_mkdir(movies);
	axis_test_storage_write(film, "frames");
	add_volume(fx, disk, "Disk");
	csrf = csrf_for(fx, fx->admin);
	before = axis_test_storage_digest(disk);

	reply = get_as(fx, fx->admin, "/storage/volumes/1");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Library folders");
	assert_contains(reply.body, "action=\"/storage/roots\"");
	assert_contains(reply.body, "/storage/volumes/1/remove");
	assert_no_inline_code(reply.body);
	axis_test_reply_clear(&reply);

	/* Viewers cannot add. */
	form = form_of("volume", "1", "path", "Movies", "type", "movies", "name", "", "return", "volume",
		NULL);
	{
		g_autofree gchar *viewer_csrf = csrf_for(fx, fx->viewer);

		reply = post_as(fx, fx->viewer, "/storage/roots", viewer_csrf, form);
		g_assert_cmpuint(reply.status, ==, 403);
		axis_test_reply_clear(&reply);
	}

	reply = post_as(fx, fx->admin, "/storage/roots", csrf, form);
	g_assert_cmpuint(reply.status, ==, 303);
	g_assert_cmpstr(axis_test_reply_header(&reply, "Location"), ==,
		"/storage/volumes/1?done=root-added&at=volume-roots#volume-roots");
	axis_test_reply_clear(&reply);
	count = axis_test_db_query(fx->db, "SELECT rel_path || '/' || media_type FROM library_root");
	g_assert_cmpstr(count, ==, "Movies/movies");

	/* An escape is refused with its reason. */
	g_clear_pointer(&form, g_free);
	form = form_of("volume", "1", "path", "../elsewhere", "type", "tv", NULL);
	reply = post_as(fx, fx->admin, "/storage/roots", csrf, form);
	g_assert_cmpuint(reply.status, ==, 400);
	assert_contains(reply.body, ". or .. segment");
	axis_test_reply_clear(&reply);

	reply = get_as(fx, fx->viewer, "/storage/roots");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Movies");
	assert_lacks(reply.body, "/storage/roots/1/remove");
	axis_test_reply_clear(&reply);

	reply = get_as(fx, fx->admin, "/storage/roots/1/remove");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Nothing on the disk is deleted or changed");
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, "/storage/roots/1/remove", csrf, "confirm=yes");
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	g_clear_pointer(&count, g_free);
	count = axis_test_db_query(fx->db, "SELECT count(*) FROM library_root");
	g_assert_cmpstr(count, ==, "0");

	after = axis_test_storage_digest(disk);
	g_assert_cmpstr(before, ==, after);
}

/* Spec: removing an offline volume needs saying the disk is gone */
static void
test_remove_offline_volume(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *disk = make_disk(fx, "disk");
	g_autofree gchar *csrf = NULL;
	g_autofree gchar *count = NULL;
	AxisTestReply reply;

	SKIP_WITHOUT_DB(fx);
	add_volume(fx, disk, "Disk");
	unmount_disk(fx, disk, "disk");
	check_now(fx, 1);
	csrf = csrf_for(fx, fx->admin);

	reply = get_as(fx, fx->viewer, "/storage/volumes/1/remove");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);

	reply = get_as(fx, fx->admin, "/storage/volumes/1/remove");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "not online now");
	assert_contains(reply.body, "name=\"offline_ok\"");
	axis_test_reply_clear(&reply);

	reply = post_as(fx, fx->admin, "/storage/volumes/1/remove", csrf, "confirm=yes");
	g_assert_cmpuint(reply.status, ==, 409);
	axis_test_reply_clear(&reply);
	count = axis_test_db_query(fx->db, "SELECT count(*) FROM volume");
	g_assert_cmpstr(count, ==, "1");

	reply = post_as(fx, fx->admin, "/storage/volumes/1/remove", csrf, "confirm=yes&offline_ok=yes");
	g_assert_cmpuint(reply.status, ==, 303);
	g_assert_cmpstr(axis_test_reply_header(&reply, "Location"), ==, "/storage?done=removed");
	axis_test_reply_clear(&reply);
	g_clear_pointer(&count, g_free);
	count = axis_test_db_query(fx->db, "SELECT count(*) FROM volume");
	g_assert_cmpstr(count, ==, "0");
	g_assert_cmpuint(log_mentions("audit: volume removed: id=1"), ==, 1);
}

/* Spec: The volume's page says what it is recognised by; the marker is
 * written when the disk is writable.  The read-only mount itself is in
 * test-storage-mounts.c: here the row says the volume was added without a
 * marker, as from a read-only disk, and the directory is writable. */
static void
test_volume_without_marker(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *disk = make_disk(fx, "archive");
	g_autofree gchar *marker = g_build_filename(disk, AXIS_VOLUME_MARKER_NAME, NULL);
	g_autofree gchar *csrf = NULL;
	g_autofree gchar *uuid = NULL;
	g_autofree gchar *sql = NULL;
	g_autoptr(AxisVolumeInfo) volume = NULL;
	g_autoptr(GError) error = NULL;
	struct stat st;
	struct statfs fs;
	struct statvfs vfs;
	AxisVolumeMarker read_back;
	AxisTestReply reply;
	gint fd;

	SKIP_WITHOUT_DB(fx);
	add_volume(fx, disk, "Archive");
	g_assert_cmpint(g_unlink(marker), ==, 0);
	g_assert_cmpint(g_stat(disk, &st), ==, 0);
	g_assert_cmpint(statfs(disk, &fs), ==, 0);
	g_assert_cmpint(statvfs(disk, &vfs), ==, 0);
	sql = g_strdup_printf("UPDATE volume SET identity = 'filesystem', marker_created_at = NULL,"
		" ident_magic = %" G_GINT64_FORMAT ", ident_fsid = '%08x%08x', ident_ino = %" G_GINT64_FORMAT ","
		" ident_bytes = %" G_GINT64_FORMAT " WHERE id = 1", (gint64)(gulong)fs.f_type,
		(guint)fs.f_fsid.__val[0], (guint)fs.f_fsid.__val[1], (gint64)st.st_ino,
		(gint64)((guint64)vfs.f_blocks * (guint64)vfs.f_frsize));
	axis_test_db_exec(fx->db, sql);
	g_assert_true(axis_storage_load(fx->storage, &error));
	g_assert_no_error(error);

	reply = get_as(fx, fx->viewer, "/storage/volumes/1");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Recognised by");
	assert_contains(reply.body, "Its filesystem identifier:");
	assert_contains(reply.body, "It has no marker yet.");
	assert_contains(reply.body, "Not yet");
	assert_contains(reply.body, "no conversion worker is given work on it");
	assert_contains(reply.body, "the first time it finds the disk mounted read-write");
	assert_no_inline_code(reply.body);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, "/storage");
	assert_contains(reply.body, "(no marker yet)");
	axis_test_reply_clear(&reply);
	/* Nothing to restore: there never was a marker. */
	reply = get_as(fx, fx->admin, "/storage/volumes/1/marker");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "added without a marker");
	assert_lacks(reply.body, "Write the marker again");
	axis_test_reply_clear(&reply);

	/* The directory is writable: Check now writes the marker, storage
	 * read-only as it is by default, and the marker takes over. */
	g_assert_true(axis_storage_is_read_only_mode(fx->storage));
	g_assert_false(g_file_test(marker, G_FILE_TEST_EXISTS));
	csrf = csrf_for(fx, fx->admin);
	reply = post_as(fx, fx->admin, "/storage/volumes/1/check", csrf, "");
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	volume = axis_storage_get_volume(fx->storage, 1);
	g_assert_true(volume->has_marker);
	fd = open(disk, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	g_assert_cmpint(fd, >=, 0);
	g_assert_cmpint(axis_volume_marker_read(fd, &read_back, NULL), ==, AXIS_VOLUME_MARKER_FOUND);
	close(fd);
	g_assert_cmpstr(read_back.uuid, ==, volume->uuid);
	uuid = axis_test_db_query(fx->db, "SELECT identity || ' ' || (marker_created_at IS NOT NULL) FROM volume");
	g_assert_cmpstr(uuid, ==, "marker true");
	reply = get_as(fx, fx->viewer, "/storage/volumes/1");
	assert_contains(reply.body, "Its marker, .axis-volume");
	assert_lacks(reply.body, "no marker yet");
	axis_test_reply_clear(&reply);

}

/*
 * Storage hunt, minor (b): the restore page does not take folders of the
 * roots' names for the volume's disk.  It compares the filesystem at the
 * volume's path with the one the volume was last seen on, says plainly when
 * it differs, and the marker is then written only with that confirmed.
 */
static void
test_restore_marker_page(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *disk = make_disk(fx, "disk");
	g_autofree gchar *movies = g_build_filename(disk, "Movies", NULL);
	g_autofree gchar *marker = g_build_filename(disk, AXIS_VOLUME_MARKER_NAME, NULL);
	g_autofree gchar *csrf = NULL;
	g_autofree gchar *dev = NULL;
	g_autofree gchar *ino = NULL;
	g_autofree gchar *form = NULL;
	g_autoptr(AxisMarkerRestoreInspection) look = NULL;
	g_autoptr(GError) error = NULL;
	AxisTestReply reply;

	SKIP_WITHOUT_DB(fx);
	axis_test_storage_mkdir(movies);
	add_volume(fx, disk, "Disk");
	g_assert_true(axis_storage_add_root(fx->storage, 1, "Movies", AXIS_MEDIA_MOVIES, NULL, NULL, &error));
	g_assert_no_error(error);
	set_writable(fx);
	g_assert_cmpint(g_unlink(marker), ==, 0);
	check_now(fx, 1);
	/* The volume was last seen on another filesystem than the one at its
	 * path now (its recorded identifier is another), the same folders. */
	axis_test_db_exec(fx->db, "UPDATE volume SET fsid = '0123456789abcdef' WHERE id = 1");
	g_assert_true(axis_storage_load(fx->storage, &error));
	g_assert_no_error(error);

	reply = get_as(fx, fx->admin, "/storage/volumes/1/marker");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "is not the one this disk was last seen on");
	assert_contains(reply.body, "they alone do not show that this is the same disk");
	assert_lacks(reply.body, "only its marker is missing");
	assert_contains(reply.body, "other_filesystem_ok");
	assert_no_inline_code(reply.body);
	axis_test_reply_clear(&reply);

	/* Posted without that confirmed: refused, nothing written. */
	g_assert_true(axis_storage_inspect_marker_restore(fx->storage, 1, &look, &error));
	g_assert_no_error(error);
	dev = g_strdup_printf("%" G_GUINT64_FORMAT, look->dev);
	ino = g_strdup_printf("%" G_GUINT64_FORMAT, look->ino);
	csrf = csrf_for(fx, fx->admin);
	form = form_of("dev", dev, "ino", ino, "fsid", look->fsid, "confirm", "yes", NULL);
	reply = post_as(fx, fx->admin, "/storage/volumes/1/marker", csrf, form);
	g_assert_cmpuint(reply.status, ==, 409);
	assert_contains(reply.body, "not the one this disk was last seen on");
	axis_test_reply_clear(&reply);
	g_assert_false(g_file_test(marker, G_FILE_TEST_EXISTS));

	/* Confirmed: written, and the volume is online. */
	g_free(form);
	form = form_of("dev", dev, "ino", ino, "fsid", look->fsid, "confirm", "yes", "other_filesystem_ok", "yes", NULL);
	reply = post_as(fx, fx->admin, "/storage/volumes/1/marker", csrf, form);
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	g_assert_true(g_file_test(marker, G_FILE_TEST_IS_REGULAR));

	/* Its own filesystem, as recorded: said so; nothing more to confirm
	 * where that filesystem has an identifier of its own. */
	g_assert_cmpint(g_unlink(marker), ==, 0);
	check_now(fx, 1);
	reply = get_as(fx, fx->admin, "/storage/volumes/1/marker");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_lacks(reply.body, "is not the one this disk was last seen on");
	axis_test_reply_clear(&reply);
}

/* The identify plan is never taken for a directory that is writable: the
 * confirmation does not match what is there, and nothing is added. */
static void
test_identify_refused_when_writable(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *disk = make_disk(fx, "plain");
	g_autofree gchar *csrf = NULL;
	g_autofree gchar *form = NULL;
	g_autofree gchar *count = NULL;
	g_autofree gchar *base = NULL;
	gchar **parts;
	AxisTestReply reply;

	SKIP_WITHOUT_DB(fx);
	/* The form the page would post, with the other plan. */
	base = confirm_form(disk, "Plain", TRUE);
	parts = g_strsplit(base, "plan=write", 2);
	g_assert_cmpuint(g_strv_length(parts), ==, 2);
	form = g_strjoinv("plan=identify", parts);
	g_strfreev(parts);
	csrf = csrf_for(fx, fx->admin);
	reply = post_as(fx, fx->admin, "/storage/volumes", csrf, form);
	g_assert_cmpuint(reply.status, ==, 409);
	assert_contains(reply.body, "has changed since it was shown");
	axis_test_reply_clear(&reply);
	count = axis_test_db_query(fx->db, "SELECT count(*) FROM volume");
	g_assert_cmpstr(count, ==, "0");
	{
		g_autofree gchar *marker = g_build_filename(disk, AXIS_VOLUME_MARKER_NAME, NULL);

		g_assert_false(g_file_test(marker, G_FILE_TEST_EXISTS));
	}
}

/* ── Live ───────────────────────────────────────────────────────── */

static void
test_live_state(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *disk = make_disk(fx, "disk");
	g_autofree gchar *csrf = NULL;
	g_autoptr(AxisTestStream) stream = NULL;
	AxisTestReply reply;

	SKIP_WITHOUT_DB(fx);
	add_volume(fx, disk, "Disk");
	stream = axis_test_stream_open(fx->port, "/events?topic=storage", fx->viewer, 0);
	g_assert_cmpuint(axis_test_stream_get_status(stream), ==, 200);
	g_assert_true(axis_test_stream_wait_for(stream, ": open", AXIS_TEST_WAIT_MS));

	/* The disk goes; an administrator checks it now; the viewer's page
	 * hears of it. */
	unmount_disk(fx, disk, "disk");
	csrf = csrf_for(fx, fx->admin);
	reply = post_as(fx, fx->admin, "/storage/volumes/1/check", csrf, "");
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	g_assert_true(axis_test_stream_wait_for(stream, "id=\"volume-1-state\"", AXIS_TEST_WAIT_MS));
	g_assert_true(axis_test_stream_wait_for(stream, "state-offline", AXIS_TEST_WAIT_MS));
	g_assert_true(axis_test_stream_wait_for(stream, "hx-swap-oob", AXIS_TEST_WAIT_MS));
}

/* The scheduled check runs from the job queue. */
static void
test_check_job(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *disk = make_disk(fx, "disk");
	g_autoptr(AxisJobRegistry) jobs = NULL;
	g_autoptr(AxisJobQueue) queue = NULL;
	g_autoptr(GError) error = NULL;

	SKIP_WITHOUT_DB(fx);
	add_volume(fx, disk, "Disk");
	unmount_disk(fx, disk, "disk");

	jobs = axis_job_registry_new();
	g_assert_true(axis_job_registry_add_modules(jobs, fx->registry, &error));
	g_assert_no_error(error);
	g_assert_true(axis_job_registry_contains(jobs, AXIS_STORAGE_CHECK_KIND));
	queue = axis_job_queue_new(fx->pool, jobs);
	axis_job_queue_set_poll_interval(queue, 100);
	axis_job_queue_set_schedule_interval(queue, 100);
	g_assert_true(axis_job_queue_start(queue, &error));
	g_assert_no_error(error);

	/* Its default schedule fires at once and the job finds the volume
	 * offline. */
	g_assert_true(axis_test_wait_query(fx->db, "SELECT state FROM volume WHERE id = 1", "offline",
		AXIS_TEST_WAIT_MS));
	g_assert_true(axis_test_wait_query(fx->db, "SELECT run_interval::text FROM job_schedule"
		" WHERE name = 'storage.check'", "00:01:00", AXIS_TEST_WAIT_MS));
	g_assert_true(axis_test_wait_query(fx->db, "SELECT state::text FROM job WHERE kind = 'storage.check'"
		" ORDER BY id LIMIT 1", "succeeded", AXIS_TEST_WAIT_MS));

	axis_job_queue_stop(queue, 5000);
	g_assert_true(axis_job_queue_wait_threads(queue, AXIS_TEST_WAIT_MS));
	while (g_main_context_iteration(NULL, FALSE))
		;
}

/* A form's answer is a redirect to the element the person acted on, and the
 * page there draws its notice inside that element (the element comes before
 * the first notice in the page). */
static void
assert_back_to_place(
	Fixture       *fx,
	AxisTestReply *reply,
	const gchar   *place
){
	g_autofree gchar *at = g_strdup_printf("at=%s", place);
	g_autofree gchar *tail = g_strdup_printf("#%s", place);
	g_autofree gchar *id = g_strdup_printf("id=\"%s\"", place);
	g_autofree gchar *path = NULL;
	const gchar *location = axis_test_reply_header(reply, "Location");
	const gchar *element;
	const gchar *notice;
	AxisTestReply page;

	g_assert_cmpuint(reply->status, ==, 303);
	g_assert_nonnull(location);
	g_assert_nonnull(strstr(location, at));
	g_assert_true(g_str_has_suffix(location, tail));
	path = g_strndup(location, strlen(location) - strlen(tail));
	page = get_as(fx, fx->admin, path);
	g_assert_cmpuint(page.status, ==, 200);
	element = strstr(page.body, id);
	notice = strstr(page.body, "notice-here");
	if (element == NULL || notice == NULL || element > notice)
		g_error("expected the notice inside %s in:\n%s", place, page.body);
	axis_test_reply_clear(&page);
	axis_test_reply_clear(reply);
}

/* Spec: a form comes back to the card or row it was posted from */
static void
test_back_to_place(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *disk = make_disk(fx, "disk");
	g_autofree gchar *moved = g_build_filename(fx->media, "moved", NULL);
	g_autofree gchar *movies = g_build_filename(moved, "Movies", NULL);
	g_autofree gchar *shows = g_build_filename(moved, "Shows", NULL);
	g_autofree gchar *csrf = NULL;
	g_autofree gchar *form = NULL;
	AxisTestReply reply;

	SKIP_WITHOUT_DB(fx);
	add_volume(fx, disk, "Disk");
	csrf = csrf_for(fx, fx->admin);

	/* Check now: the volume page's Actions card. */
	reply = post_as(fx, fx->admin, "/storage/volumes/1/check", csrf, "");
	assert_back_to_place(fx, &reply, "volume-actions");

	/* Change the path (the disk is mounted somewhere else): the same card. */
	g_assert_cmpint(g_rename(disk, moved), ==, 0);
	form = form_of("path", moved, "not_mount_point_ok", "yes", NULL);
	reply = post_as(fx, fx->admin, "/storage/volumes/1/path", csrf, form);
	assert_back_to_place(fx, &reply, "volume-actions");
	g_clear_pointer(&form, g_free);

	/* Add a root from the volume's page: its roots card. */
	form = form_of("volume", "1", "path", "Movies", "type", "movies", "name", "", "return", "volume", NULL);
	axis_test_storage_mkdir(movies);
	reply = post_as(fx, fx->admin, "/storage/roots", csrf, form);
	assert_back_to_place(fx, &reply, "volume-roots");
	g_clear_pointer(&form, g_free);

	/* Add a root from the roots page: its Add a root card. */
	axis_test_storage_mkdir(shows);
	form = form_of("volume", "1", "path", "Shows", "type", "tv", "name", "", NULL);
	reply = post_as(fx, fx->admin, "/storage/roots", csrf, form);
	assert_back_to_place(fx, &reply, "roots-add");

	/* Remove a root, from the roots page and from the volume's. */
	reply = post_as(fx, fx->admin, "/storage/roots/2/remove", csrf, "confirm=yes");
	assert_back_to_place(fx, &reply, "roots");
	reply = post_as(fx, fx->admin, "/storage/roots/1/remove", csrf, "confirm=yes&return=volume");
	assert_back_to_place(fx, &reply, "volume-roots");

	/* The volume's page says where its scan stands (add-activity-feedback);
	 * a hidden place for the line without one. */
	reply = get_as(fx, fx->viewer, "/storage/volumes/1");
	assert_contains(reply.body, "id=\"activity-volume\"");
	assert_lacks(reply.body, "A scan of this disk is");
	axis_test_reply_clear(&reply);
	axis_test_db_exec(fx->db, "INSERT INTO job (kind, queue, state, max_attempts, dedupe_key, payload) VALUES"
		" ('library.scan', 'scan', 'queued', 3, 'library.scan:volume:1', '{\"volume_id\": 1}')");
	reply = get_as(fx, fx->viewer, "/storage/volumes/1");
	assert_contains(reply.body, "A scan of this disk is waiting for its turn, queued");
	axis_test_reply_clear(&reply);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

#define ADD_TEST(path, func) \
	g_test_add(path, Fixture, NULL, fixture_setup, func, fixture_teardown)

	ADD_TEST("/web/storage/volumes-page", test_volumes_page);
	ADD_TEST("/web/storage/dashboard-space", test_dashboard_space);
	ADD_TEST("/web/storage/names-escaped", test_names_are_escaped);
	ADD_TEST("/web/storage/add-with-confirmation", test_add_with_confirmation);
	ADD_TEST("/web/storage/add-refuses-changed-directory", test_add_refuses_changed_directory);
	ADD_TEST("/web/storage/inspect-refusals", test_inspect_refusals);
	ADD_TEST("/web/storage/roots", test_roots_pages);
	ADD_TEST("/web/storage/remove-offline-volume", test_remove_offline_volume);
	ADD_TEST("/web/storage/volume-without-marker", test_volume_without_marker);
	ADD_TEST("/web/storage/restore-marker-page", test_restore_marker_page);
	ADD_TEST("/web/storage/identify-refused-when-writable", test_identify_refused_when_writable);
	ADD_TEST("/web/storage/live-state", test_live_state);
	ADD_TEST("/web/storage/check-job", test_check_job);
	ADD_TEST("/web/storage/back-to-place", test_back_to_place);

	return g_test_run();
}
