/*
 * test-web-worker-attach.c - "Attach a worker" against PostgreSQL: the five
 * pages (administrators only, every POST under the CSRF check), the token
 * made at step 4 and nowhere before, what is refused and creates nothing,
 * and the last page: what a worker reported, asked for again when one
 * registers
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The daemon's side runs on the test's main loop (axis-test-worker.h); the
 * worker is played through the API.  Nothing here starts an axis-worker, a
 * container tool, ffmpeg or a GPU.
 */

#include <glib.h>
#include <string.h>

#include "axis-test-db.h"
#include "axis-test-jobs.h"
#include "axis-test-web.h"
#include "axis-test-worker.h"
#include "axis-version.h"
#include "web/axis-password.h"
#include "web/axis-worker-attach.h"

#define UUID_A "3f2a5c1e-7b9d-4e2f-8a6b-1c3d5e7f9a0b"
#define UUID_B "6c1d2e3f-4a5b-4c6d-9e8f-0a1b2c3d4e5f"

typedef struct
{
	AxisTestDb     *db;
	AxisDbPool     *pool;
	AxisTestDaemon *d;
	gchar          *admin;
	gchar          *viewer;
	gchar          *garage;		/* the token of a worker made before */
	gchar          *a;		/* the disks' numbers, as text */
	gchar          *b;
	gchar          *unmarked;
} Fixture;

#define SKIP_WITHOUT_DB(fx) do { if ((fx)->db == NULL) return; } while (0)

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	memset(fx, 0, sizeof(*fx));
	axis_test_log_init();
	axis_test_log_reset();
	fx->db = axis_test_db_new();
	if (fx->db == NULL)
		return;
	fx->pool = axis_test_jobs_pool(fx->db, 8);
	fx->d = axis_test_daemon_new_with_hub(fx->pool, NULL, NULL, "");
	fx->admin = axis_web_auth_open_test_session(fx->d->auth, "root", AXIS_USER_ROLE_ADMIN);
	fx->viewer = axis_web_auth_open_test_session(fx->d->auth, "viewer", AXIS_USER_ROLE_USER);
	fx->garage = axis_test_daemon_add_worker(fx->d, "garage", NULL);
	/* Two disks with their markers, named as a person might; and one added
	 * from a read-only mount, which has none yet. */
	axis_test_db_exec(fx->db, "INSERT INTO volume (uuid, name, path, mount_root, state, marker_created_at) VALUES"
		" ('" UUID_A "', 'Disk <A> & \"films\"', '/srv/a', false, 'online', now()),"
		" ('" UUID_B "', 'Disk B', '/srv/b', false, 'online', now())");
	axis_test_db_exec(fx->db, "INSERT INTO volume (uuid, name, path, mount_root, state, identity, ident_magic,"
		" ident_fsid, ident_ino, ident_bytes) VALUES (gen_random_uuid(), 'Disk C', '/srv/c', false, 'online',"
		" 'filesystem', 1, '0123456789abcdef', 2, 1000)");
	fx->a = axis_test_db_query(fx->db, "SELECT id FROM volume WHERE uuid = '" UUID_A "'");
	fx->b = axis_test_db_query(fx->db, "SELECT id FROM volume WHERE uuid = '" UUID_B "'");
	fx->unmarked = axis_test_db_query(fx->db, "SELECT id FROM volume WHERE name = 'Disk C'");
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	if (fx->db == NULL) {
		axis_test_log_reset();
		return;
	}
	axis_test_daemon_free(fx->d);
	axis_db_pool_free(fx->pool);
	axis_test_db_free(fx->db);
	axis_secret_free(fx->garage);
	g_free(fx->admin);
	g_free(fx->viewer);
	g_free(fx->a);
	g_free(fx->b);
	g_free(fx->unmarked);
	axis_test_log_reset();
}

/* ── Helpers ────────────────────────────────────────────────────── */

static void
assert_contains(
	const gchar *text,
	const gchar *needle
){
	if (text == NULL || strstr(text, needle) == NULL)
		g_error("expected \"%s\" in:\n%s", needle, text != NULL ? text : "(null)");
}

static void
assert_lacks(
	const gchar *text,
	const gchar *needle
){
	if (text != NULL && strstr(text, needle) != NULL)
		g_error("did not expect \"%s\" in:\n%s", needle, text);
}

static guint
count_of(
	const gchar *text,
	const gchar *needle
){
	guint n = 0;
	const gchar *p;

	for (p = strstr(text, needle); p != NULL; p = strstr(p + strlen(needle), needle))
		n++;
	return n;
}

static void
assert_q(
	Fixture     *fx,
	const gchar *sql,
	const gchar *expected
){
	g_autofree gchar *got = axis_test_db_query(fx->db, sql);

	if (g_strcmp0(got, expected) != 0)
		g_error("%s: expected %s, got %s", sql, expected != NULL ? expected : "NULL", got != NULL ? got : "NULL");
}

static AxisTestReply
get(
	Fixture     *fx,
	const gchar *cookie,
	const gchar *path
){
	return axis_test_web_request(fx->d->session, fx->d->port, "GET", path, cookie, NULL, NULL);
}

/* A part of a page, as htmx asks for it. */
static AxisTestReply
get_part(
	Fixture     *fx,
	const gchar *cookie,
	const gchar *path
){
	static const gchar *const extra[] = { "HX-Request", "true", NULL };

	return axis_test_web_request(fx->d->session, fx->d->port, "GET", path, cookie, extra, NULL);
}

/* A form posted with the session's own CSRF token. */
static AxisTestReply
post(
	Fixture     *fx,
	const gchar *cookie,
	const gchar *path,
	const gchar *form
){
	AxisTestReply page = get(fx, cookie, cookie == fx->admin ? "/workers" : "/");
	g_autofree gchar *csrf = axis_test_web_csrf_token(page.body);
	const gchar *extra[3];

	axis_test_reply_clear(&page);
	extra[0] = "X-CSRF-Token";
	extra[1] = csrf != NULL ? csrf : "none";
	extra[2] = NULL;
	return axis_test_web_request(fx->d->session, fx->d->port, "POST", path, cookie, extra, form);
}

/* The token on the page of step 4. */
static gchar *
token_on_page(const gchar *html)
{
	const gchar *start = strstr(html, "worker-token");
	const gchar *end;

	g_assert_nonnull(start);
	start = strstr(start, "<code>");
	g_assert_nonnull(start);
	start += strlen("<code>");
	end = strstr(start, "</code>");
	g_assert_nonnull(end);
	return g_strndup(start, end - start);
}

/* A page of a step before the fourth: no worker has been made (so there is
 * no token to show), and the page has no place for one.  (A page's own CSRF
 * token has a token's shape, so the shape proves nothing here; that the
 * generated text holds none is test-worker-attach.c's.) */
static void
assert_no_token_yet(
	Fixture     *fx,
	const gchar *html
){
	assert_lacks(html, "worker-token");
	assert_lacks(html, "name=\"token\"");
	assert_q(fx, "SELECT string_agg(name, ',' ORDER BY name) FROM worker", "garage");
}

/* Step 1's fields, for a worker called @name. */
static gchar *
machine(
	const gchar *name,
	const gchar *tool,
	const gchar *encoder,
	const gchar *url
){
	g_autofree gchar *escaped = g_uri_escape_string(url, NULL, FALSE);
	g_autofree gchar *who = g_uri_escape_string(name, NULL, FALSE);

	return g_strdup_printf("name=%s&tool=%s&encoder=%s&arch=x86-64&url=%s", who, tool, encoder, escaped);
}

/* Step 2's fields for one disk. */
static gchar *
disk(
	const gchar *number,
	const gchar *kind,
	const gchar *host
){
	g_autofree gchar *escaped = g_uri_escape_string(host, NULL, FALSE);

	return g_strdup_printf("&disk-%s=yes&kind-%s=%s&host-%s=%s", number, number, kind, number, escaped);
}

/* ── Who may ────────────────────────────────────────────────────── */

/* Spec: administrators only; every POST is refused without the session's
 * CSRF token */
static void
test_who_may(Fixture *fx, gconstpointer data)
{
	static const gchar *const posts[] = { "/workers/attach", "/workers/attach/disks", "/workers/attach/mounts",
		"/workers/attach/create" };
	static const gchar *const gets[] = { "/workers/attach", "/workers/attach/waiting?name=garage&encoder=cpu",
		"/workers/attach/status?name=garage&encoder=cpu" };
	g_autofree gchar *a = NULL;
	g_autofree gchar *m = NULL;
	g_autofree gchar *all = NULL;
	AxisTestReply reply;
	guint i;

	SKIP_WITHOUT_DB(fx);
	a = disk(fx->a, "nfs", "/mnt/axis/a");
	m = machine("intruder", "docker", "cpu", "https://axis.example.org");
	all = g_strconcat(m, a, NULL);
	for (i = 0; i < G_N_ELEMENTS(gets); i++) {
		reply = get(fx, fx->viewer, gets[i]);
		g_assert_cmpuint(reply.status, ==, 403);
		assert_lacks(reply.body, "garage");
		axis_test_reply_clear(&reply);
		/* Nobody: sent to the login page. */
		reply = get(fx, NULL, gets[i]);
		g_assert_cmpuint(reply.status, !=, 200);
		axis_test_reply_clear(&reply);
	}
	for (i = 0; i < G_N_ELEMENTS(posts); i++) {
		/* Not an administrator. */
		reply = post(fx, fx->viewer, posts[i], all);
		g_assert_cmpuint(reply.status, ==, 403);
		axis_test_reply_clear(&reply);
		/* An administrator's session without its CSRF token. */
		reply = axis_test_web_request(fx->d->session, fx->d->port, "POST", posts[i], fx->admin, NULL, all);
		g_assert_cmpuint(reply.status, ==, 403);
		assert_lacks(reply.body, "worker-token");
		axis_test_reply_clear(&reply);
		/* With a token that is not the session's. */
		{
			static const gchar *const wrong[] = { "X-CSRF-Token", "bm90LXRoZS1zZXNzaW9ucy10b2tlbg", NULL };

			reply = axis_test_web_request(fx->d->session, fx->d->port, "POST", posts[i], fx->admin, wrong, all);
			g_assert_cmpuint(reply.status, ==, 403);
			axis_test_reply_clear(&reply);
		}
	}
	assert_q(fx, "SELECT string_agg(name, ',' ORDER BY name) FROM worker", "garage");

	/* The Conversion workers page leads to the guide, and keeps the old way. */
	reply = get(fx, fx->admin, "/workers");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "href=\"/workers/attach\"");
	assert_contains(reply.body, "Add a worker without the guide");
	axis_test_reply_clear(&reply);
}

/* ── The five pages ─────────────────────────────────────────────── */

/* Spec: the token is created at step 4 and is on no page, in no field and
 * in no address before */
static void
test_steps(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *m = machine("  Shed-PC ", "quadlet", "nvenc", "https://axis.example.org/");
	g_autofree gchar *a = NULL;
	g_autofree gchar *b = NULL;
	g_autofree gchar *all = NULL;
	g_autofree gchar *token = NULL;
	g_autofree gchar *hash = NULL;
	g_autofree gchar *offered = NULL;
	g_autofree gchar *waiting = NULL;
	g_autofree gchar *without = NULL;
	g_autofree gchar *volume_a = NULL;
	g_autofree gchar *tick_a = NULL;
	AxisTestReply reply;

	SKIP_WITHOUT_DB(fx);
	a = disk(fx->a, "nfs", " /mnt/axis/films/ ");
	b = disk(fx->b, "sshfs", "/mnt/axis/b");
	all = g_strconcat(m, a, b, NULL);
	tick_a = g_strdup_printf("name=\"disk-%s\"", fx->a);

	/* Step 1: the address the browser used is offered, and nothing else of
	 * this host. */
	reply = get(fx, fx->admin, "/workers/attach");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Step 1 of 5");
	offered = g_strdup_printf("value=\"http://127.0.0.1:%u\"", (guint)fx->d->port);
	assert_contains(reply.body, offered);
	assert_contains(reply.body, "action=\"/workers/attach/disks\"");
	assert_no_token_yet(fx, reply.body);
	axis_test_reply_clear(&reply);

	/* Step 2: the disks Axis knows, their names as text. */
	reply = post(fx, fx->admin, "/workers/attach/disks", m);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Step 2 of 5");
	assert_contains(reply.body, "Disk &lt;A&gt; &amp; ");
	assert_lacks(reply.body, "Disk <A>");
	assert_contains(reply.body, "Disk B");
	assert_contains(reply.body, tick_a);
	/* The disk without a marker is named and cannot be ticked. */
	assert_contains(reply.body, "Disk C");
	{
		g_autofree gchar *tick_c = g_strdup_printf("name=\"disk-%s\"", fx->unmarked);

		assert_lacks(reply.body, tick_c);
	}
	assert_contains(reply.body, "value=\"  Shed-PC \"");
	assert_no_token_yet(fx, reply.body);
	axis_test_reply_clear(&reply);

	/* Step 3: the mount commands, with placeholders for what Axis does not
	 * know. */
	reply = post(fx, fx->admin, "/workers/attach/mounts", all);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Step 3 of 5");
	assert_contains(reply.body, "sudo mount -t nfs4 -o rw,hard SERVER:/PATH/ON/SERVER /mnt/axis/films\n");
	assert_contains(reply.body, "sshfs AXIS_USER@SERVER:/PATH/ON/SERVER /mnt/axis/b ");
	assert_contains(reply.body, "action=\"/workers/attach/create\"");
	assert_contains(reply.body, "action=\"/workers/attach/disks\"");
	assert_lacks(reply.body, "/srv/a");
	assert_lacks(reply.body, "127.0.0.1");
	assert_no_token_yet(fx, reply.body);
	axis_test_reply_clear(&reply);

	/* Going back keeps what was chosen. */
	reply = post(fx, fx->admin, "/workers/attach/disks", all);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "value=\" /mnt/axis/films/ \"");
	assert_contains(reply.body, "<option selected=\"selected\" value=\"sshfs\">");
	axis_test_reply_clear(&reply);
	reply = post(fx, fx->admin, "/workers/attach", all);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Step 1 of 5");
	assert_contains(reply.body, "<option selected=\"selected\" value=\"quadlet\">");
	assert_contains(reply.body, "<option selected=\"selected\" value=\"nvenc\">");
	assert_contains(reply.body, tick_a);
	axis_test_reply_clear(&reply);

	/* Nothing was created by any of that. */
	assert_q(fx, "SELECT string_agg(name, ',' ORDER BY name) FROM worker", "garage");

	/* Step 4: the worker, its token (once), and the text for Quadlet. */
	reply = post(fx, fx->admin, "/workers/attach/create", all);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Step 4 of 5");
	assert_contains(reply.body, "Worker shed-pc");
	token = token_on_page(reply.body);
	g_assert_cmpuint(strlen(token), ==, 43);
	g_assert_cmpuint(count_of(reply.body, token), ==, 1);
	g_assert_null(axis_test_reply_header(&reply, "Set-Cookie"));
	assert_contains(reply.body, "Save this as axis-worker-shed-pc.container.");
	assert_contains(reply.body, "Image=" AXIS_ATTACH_IMAGE ":" AXIS_VERSION "\n");
	assert_contains(reply.body, "Environment=AXIS_WORKER_URL=https://axis.example.org\n");
	volume_a = g_strdup_printf("Volume=/mnt/axis/films:/media/disk-%s\n", fx->a);
	assert_contains(reply.body, volume_a);
	without = g_strdup_printf("Environment=AXIS_WORKER_VOLUMES_WITHOUT_FILE_IDENTITY=/media/disk-%s\n", fx->b);
	assert_contains(reply.body, without);
	assert_contains(reply.body, "Environment=AXIS_WORKER_ENCODER_FAMILIES=nvenc\n");
	assert_contains(reply.body, "AddDevice=nvidia.com/gpu=all\n");
	assert_contains(reply.body, "ReadOnly=true\n");
	assert_contains(reply.body, "DropCapability=all\n");
	assert_contains(reply.body, "NoNewPrivileges=true\n");
	assert_contains(reply.body, "shed-pc.token:/run/secrets/axis_worker_token:ro\n");
	assert_contains(reply.body, "cat &gt; ~/.config/axis-worker/shed-pc.token");
	assert_contains(reply.body, "systemctl --user start axis-worker-shed-pc\n");
	/* The way on names the worker, its encoder and its disks, not the token. */
	waiting = g_strdup_printf("href=\"/workers/attach/waiting?name=shed-pc&amp;encoder=nvenc&amp;disks=%s.%s\"",
		g_ascii_strtoll(fx->a, NULL, 10) < g_ascii_strtoll(fx->b, NULL, 10) ? fx->a : fx->b,
		g_ascii_strtoll(fx->a, NULL, 10) < g_ascii_strtoll(fx->b, NULL, 10) ? fx->b : fx->a);
	assert_contains(reply.body, waiting);
	axis_test_reply_clear(&reply);

	/* Only its hash is kept, and it works. */
	hash = axis_token_hash(token);
	{
		g_autofree gchar *sql = g_strdup_printf("SELECT name FROM worker WHERE token_hash = '%s'", hash);

		assert_q(fx, sql, "shed-pc");
	}
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*audit: worker shed-pc created by root*"), ==, 1);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*worker shed-pc created with the guide: tool=quadlet "
		"encoder=nvenc disks=2*"), ==, 1);
	{
		g_autofree gchar *pattern = g_strdup_printf("*%s*", token);

		g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MASK, pattern), ==, 0);
	}

	/* Never again: the same form does not show it a second time, and the
	 * pages do not hold it. */
	reply = post(fx, fx->admin, "/workers/attach/create", all);
	g_assert_cmpuint(reply.status, ==, 400);
	assert_contains(reply.body, "Step 1 of 5");
	assert_contains(reply.body, "There is a worker of that name already");
	assert_lacks(reply.body, token);
	assert_lacks(reply.body, "worker-token");
	axis_test_reply_clear(&reply);
	reply = get(fx, fx->admin, "/workers");
	assert_contains(reply.body, "shed-pc");
	assert_lacks(reply.body, token);
	axis_test_reply_clear(&reply);
	{
		g_autofree gchar *path = g_strdup_printf("/workers/attach/waiting?name=shed-pc&encoder=nvenc&disks=%s.%s",
			fx->a, fx->b);

		reply = get(fx, fx->admin, path);
		g_assert_cmpuint(reply.status, ==, 200);
		assert_lacks(reply.body, token);
		axis_test_reply_clear(&reply);
	}
	assert_q(fx, "SELECT count(*) FROM worker", "2");
	axis_secret_free(g_steal_pointer(&token));
}

/* Spec: each tool's text on the page of step 4 */
static void
test_each_tool(Fixture *fx, gconstpointer data)
{
	static const struct
	{
		const gchar *name;
		const gchar *tool;
		const gchar *encoder;
		const gchar *has;
		const gchar *also;
	} cases[] = {
		{ "one", "docker", "cpu", "docker run -d --name axis-worker-one ", "AXIS_WORKER_ENCODER_FAMILIES=software" },
		{ "two", "compose", "vaapi", "container_name: axis-worker-two\n", "/dev/dri/renderD128:/dev/dri/renderD128" },
		{ "three", "podman", "nvenc", "podman run -d --name axis-worker-three ", "--device nvidia.com/gpu=all" },
		{ "four", "quadlet", "vaapi", "ContainerName=axis-worker-four\n", "AddDevice=/dev/dri/renderD128" }
	};
	guint i;

	SKIP_WITHOUT_DB(fx);
	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		g_autofree gchar *m = machine(cases[i].name, cases[i].tool, cases[i].encoder, "https://axis.example.org");
		g_autofree gchar *a = disk(fx->a, "bind", "/srv/films");
		g_autofree gchar *all = g_strconcat(m, a, NULL);
		g_autofree gchar *token = NULL;
		AxisTestReply reply = post(fx, fx->admin, "/workers/attach/create", all);

		g_assert_cmpuint(reply.status, ==, 200);
		assert_contains(reply.body, cases[i].has);
		assert_contains(reply.body, cases[i].also);
		assert_contains(reply.body, AXIS_ATTACH_IMAGE ":" AXIS_VERSION);
		assert_lacks(reply.body, "WITHOUT_FILE_IDENTITY");
		token = token_on_page(reply.body);
		g_assert_cmpuint(count_of(reply.body, token), ==, 1);
		axis_secret_free(g_steal_pointer(&token));
		axis_test_reply_clear(&reply);
	}
	assert_q(fx, "SELECT count(*) FROM worker", "5");
}

/* ── What is refused ────────────────────────────────────────────── */

static void
assert_refused(
	Fixture     *fx,
	const gchar *path,
	const gchar *form,
	const gchar *step,
	const gchar *says
){
	AxisTestReply reply = post(fx, fx->admin, path, form);

	if (reply.status != 400)
		g_error("%s with %s: expected 400, got %u", path, form, reply.status);
	assert_contains(reply.body, step);
	assert_contains(reply.body, "role=\"alert\"");
	if (says != NULL)
		assert_contains(reply.body, says);
	assert_lacks(reply.body, "worker-token");
	assert_lacks(reply.body, "attach-text");
	axis_test_reply_clear(&reply);
}

/* Spec: nothing typed reaches the generated text without the form's check;
 * a refusal shows the step whose input is wrong and creates nothing */
static void
test_refused(Fixture *fx, gconstpointer data)
{
	static const gchar *const names[] = { "", "9lives", "desk;reboot", "desk$(id)", "desk\nImage=evil", "garage" };
	static const gchar *const urls[] = { "", "axis.example.org", "ftp://axis.example.org",
		"https://axis.example.org';reboot;'", "https://axis.example.org/$(id)", "https://axis.example.org\" -v /:/host",
		"https://axis.example.org/a\nPodmanArgs=--privileged", "https://user:secret@axis.example.org",
		"https://axis.example.org/%h", "http://192.0.2.10:7800" };
	static const gchar *const hosts[] = { "", "mnt/axis", "/", "/mnt/../etc", "/mnt/my disk", "/mnt/a:/etc",
		"/mnt/$(reboot)", "/mnt/a\nVolume=/:/host", "/mnt/a,/etc", "/mnt/%h", "/mnt/a'" };
	g_autofree gchar *good_machine = machine("desk", "podman", "cpu", "https://axis.example.org");
	g_autofree gchar *good_disk = NULL;
	guint i;

	SKIP_WITHOUT_DB(fx);
	good_disk = disk(fx->a, "nfs", "/mnt/axis/a");

	/* Step 1's input, asked for at every later step. */
	for (i = 0; i < G_N_ELEMENTS(names); i++) {
		g_autofree gchar *m = machine(names[i], "podman", "cpu", "https://axis.example.org");
		g_autofree gchar *all = g_strconcat(m, good_disk, NULL);

		assert_refused(fx, "/workers/attach/disks", m, "Step 1 of 5", NULL);
		assert_refused(fx, "/workers/attach/mounts", all, "Step 1 of 5", NULL);
		assert_refused(fx, "/workers/attach/create", all, "Step 1 of 5", NULL);
	}
	for (i = 0; i < G_N_ELEMENTS(urls); i++) {
		g_autofree gchar *m = machine("desk", "podman", "cpu", urls[i]);
		g_autofree gchar *all = g_strconcat(m, good_disk, NULL);

		assert_refused(fx, "/workers/attach/disks", m, "Step 1 of 5", NULL);
		assert_refused(fx, "/workers/attach/create", all, "Step 1 of 5", NULL);
	}
	{
		g_autofree gchar *m = machine("desk", "kubernetes", "cpu", "https://axis.example.org");
		g_autofree gchar *e = machine("desk", "podman", "quantum", "https://axis.example.org");
		g_autofree gchar *http = machine("desk", "podman", "cpu", "http://192.0.2.10:7800");
		g_autofree gchar *allowed = g_strconcat(http, "&allow_http=yes", NULL);
		AxisTestReply reply;

		assert_refused(fx, "/workers/attach/disks", m, "Step 1 of 5", "from the lists");
		assert_refused(fx, "/workers/attach/disks", e, "Step 1 of 5", "from the lists");
		assert_refused(fx, "/workers/attach/disks", "", "Step 1 of 5", NULL);
		assert_refused(fx, "/workers/attach/disks", http, "Step 1 of 5", "plain http to another machine");
		/* Told that the network is trusted, it goes on. */
		reply = post(fx, fx->admin, "/workers/attach/disks", allowed);
		g_assert_cmpuint(reply.status, ==, 200);
		assert_contains(reply.body, "Step 2 of 5");
		axis_test_reply_clear(&reply);
	}

	/* Step 2's input. */
	assert_refused(fx, "/workers/attach/mounts", good_machine, "Step 2 of 5", "Tick at least one disk");
	assert_refused(fx, "/workers/attach/create", good_machine, "Step 2 of 5", "Tick at least one disk");
	for (i = 0; i < G_N_ELEMENTS(hosts); i++) {
		g_autofree gchar *d = disk(fx->a, "nfs", hosts[i]);
		g_autofree gchar *all = g_strconcat(good_machine, d, NULL);

		assert_refused(fx, "/workers/attach/mounts", all, "Step 2 of 5", NULL);
		assert_refused(fx, "/workers/attach/create", all, "Step 2 of 5", NULL);
	}
	{
		g_autofree gchar *kind = disk(fx->a, "floppy", "/mnt/axis/a");
		g_autofree gchar *gone = disk("999999", "nfs", "/mnt/axis/z");
		g_autofree gchar *unmarked = disk(fx->unmarked, "nfs", "/mnt/axis/c");
		g_autofree gchar *same = disk(fx->b, "nfs", "/mnt/axis/a");
		g_autofree gchar *with_kind = g_strconcat(good_machine, kind, NULL);
		g_autofree gchar *with_gone = g_strconcat(good_machine, good_disk, gone, NULL);
		g_autofree gchar *with_unmarked = g_strconcat(good_machine, unmarked, NULL);
		g_autofree gchar *with_same = g_strconcat(good_machine, good_disk, same, NULL);
		g_autofree gchar *odd = g_strconcat(good_machine, good_disk, "&disk-0=yes&disk--3=yes&disk-1e3=yes&disk-=yes"
			"&disk-007=yes&disk-99999999999999999999=yes", NULL);
		AxisTestReply reply;

		assert_refused(fx, "/workers/attach/create", with_kind, "Step 2 of 5", "from the list");
		assert_refused(fx, "/workers/attach/create", with_gone, "Step 2 of 5", "not a disk Axis knows");
		assert_refused(fx, "/workers/attach/create", with_unmarked, "Step 2 of 5", "no marker file yet");
		assert_refused(fx, "/workers/attach/create", with_same, "Step 2 of 5", "same path");
		/* Fields that are not a disk's are not fields. */
		reply = post(fx, fx->admin, "/workers/attach/mounts", odd);
		g_assert_cmpuint(reply.status, ==, 200);
		assert_contains(reply.body, "Step 3 of 5");
		axis_test_reply_clear(&reply);
	}

	/* More disks than a worker takes. */
	{
		GString *many = g_string_new(good_machine);
		guint n;

		for (n = 1; n <= AXIS_ATTACH_MAX_DISKS + 8; n++)
			g_string_append_printf(many, "&disk-%u=yes&kind-%u=nfs&host-%u=/mnt/d%u", 1000 + n, 1000 + n, 1000 + n, n);
		assert_refused(fx, "/workers/attach/create", many->str, "Step 2 of 5", NULL);
		g_string_free(many, TRUE);
	}

	/* A field longer than a field may be counts as not given. */
	{
		g_autofree gchar *long_name = g_strnfill(600, 'a');
		g_autofree gchar *m = machine(long_name, "podman", "cpu", "https://axis.example.org");

		assert_refused(fx, "/workers/attach/disks", m, "Step 1 of 5", NULL);
	}

	/* None of it created a worker. */
	assert_q(fx, "SELECT string_agg(name, ',' ORDER BY name) FROM worker", "garage");
}

/* ── The last page ──────────────────────────────────────────────── */

static void
do_register(
	Fixture            *fx,
	const gchar        *token,
	const gchar *const *volumes
){
	g_autofree gchar *json = axis_test_register_json(FALSE, volumes);
	AxisTestReply reply = axis_test_daemon_post(fx->d, "POST", "register", token, NULL, json, NULL);

	if (reply.status != 200)
		g_error("register: %u %s", reply.status, reply.body);
	axis_test_reply_clear(&reply);
}

/* Spec: the last page says what the worker reported, each ticked disk as
 * proved or not, and what to do; it is told when a worker registers */
static void
test_waiting(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *m = machine("desk", "docker", "cpu", "https://axis.example.org");
	g_autofree gchar *a = NULL;
	g_autofree gchar *b = NULL;
	g_autofree gchar *all = NULL;
	g_autofree gchar *token = NULL;
	g_autofree gchar *query = NULL;
	g_autofree gchar *page = NULL;
	g_autofree gchar *part = NULL;
	g_autofree gchar *part_nvenc = NULL;
	g_autofree gchar *inside_a = NULL;
	g_autofree gchar *inside_b = NULL;
	g_autoptr(AxisTestStream) stream = NULL;
	AxisTestReply reply;

	SKIP_WITHOUT_DB(fx);
	a = disk(fx->a, "nfs", "/mnt/axis/a");
	b = disk(fx->b, "nfs", "/mnt/axis/b");
	all = g_strconcat(m, a, b, NULL);
	query = g_strdup_printf("name=desk&encoder=cpu&disks=%s.%s", fx->a, fx->b);
	page = g_strdup_printf("/workers/attach/waiting?%s", query);
	part = g_strdup_printf("/workers/attach/status?%s", query);
	part_nvenc = g_strdup_printf("/workers/attach/status?name=desk&encoder=nvenc&disks=%s.%s", fx->a, fx->b);
	inside_a = g_strdup_printf("/media/disk-%s", fx->a);
	inside_b = g_strdup_printf("/media/disk-%s", fx->b);

	reply = post(fx, fx->admin, "/workers/attach/create", all);
	g_assert_cmpuint(reply.status, ==, 200);
	token = token_on_page(reply.body);
	axis_test_reply_clear(&reply);

	/* A worker that is not one; a page asked for without its encoder. */
	reply = get(fx, fx->admin, "/workers/attach/waiting?name=nobody&encoder=cpu");
	g_assert_cmpuint(reply.status, ==, 404);
	axis_test_reply_clear(&reply);
	reply = get(fx, fx->admin, "/workers/attach/waiting?name=<script>&encoder=cpu");
	g_assert_cmpuint(reply.status, ==, 404);
	assert_lacks(reply.body, "<script>&");
	axis_test_reply_clear(&reply);
	reply = get(fx, fx->admin, "/workers/attach/waiting?name=desk&encoder=quantum");
	g_assert_cmpuint(reply.status, ==, 400);
	axis_test_reply_clear(&reply);

	/* Nothing heard yet: the page waits, follows the workers' topic, and
	 * asks again by itself. */
	reply = get(fx, fx->admin, page);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Step 5 of 5");
	assert_contains(reply.body, "Waiting for the worker");
	assert_contains(reply.body, "Nothing has been heard from it yet.");
	assert_contains(reply.body, "id=\"attach-status\"");
	assert_contains(reply.body, "sse:workers-changed");
	assert_contains(reply.body, "every 5s");
	assert_contains(reply.body, "topic=workers:admin");
	{
		g_autofree gchar *source = g_strdup_printf("hx-get=\"/workers/attach/status?name=desk&amp;encoder=cpu&amp;"
			"disks=%s.%s\"", fx->a, fx->b);

		assert_contains(reply.body, source);
	}
	assert_lacks(reply.body, "The worker has not registered.");
	assert_lacks(reply.body, token);
	axis_test_reply_clear(&reply);

	/* The part alone, as htmx asks for it; without htmx, the page. */
	reply = get_part(fx, fx->admin, part);
	g_assert_cmpuint(reply.status, ==, 200);
	g_assert_true(g_str_has_prefix(reply.body, "<div "));
	assert_contains(reply.body, "id=\"attach-status\"");
	assert_lacks(reply.body, "<html");
	axis_test_reply_clear(&reply);
	reply = get(fx, fx->admin, part);
	g_assert_cmpuint(reply.status, ==, 303);
	assert_contains(axis_test_reply_header(&reply, "Location"), "/workers/attach/waiting?name=desk&encoder=cpu");
	axis_test_reply_clear(&reply);

	/* After a while without a registration: what to check. */
	axis_test_db_exec(fx->db, "UPDATE worker SET created_at = now() - interval '10 minutes' WHERE name = 'desk'");
	reply = get_part(fx, fx->admin, part);
	assert_contains(reply.body, "The worker has not registered.");
	assert_contains(reply.body, "The address:");
	assert_contains(reply.body, "The token:");
	assert_contains(reply.body, "every 5s");
	axis_test_reply_clear(&reply);

	/* Only administrators may follow the topic. */
	{
		g_autoptr(AxisTestStream) refused = axis_test_stream_open(fx->d->port, "/events?topic=workers:admin",
			fx->viewer, 0);

		g_assert_cmpuint(axis_test_stream_get_status(refused), ==, 403);
	}
	stream = axis_test_stream_open(fx->d->port, "/events?topic=workers:admin", fx->admin, 0);
	g_assert_cmpuint(axis_test_stream_get_status(stream), ==, 200);
	g_assert_cmpuint(axis_test_stream_count(stream, "event: workers-changed"), ==, 0);

	/* It registers with one of the two disks: the page is told, and says
	 * which disk is proved and what to do about the other. */
	{
		const gchar *const volumes[] = { UUID_A, inside_a, NULL };

		do_register(fx, token, volumes);
	}
	g_assert_true(axis_test_stream_wait_for(stream, "event: workers-changed", 5000));
	assert_lacks(axis_test_stream_get_text(stream), token);
	reply = get_part(fx, fx->admin, part);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Connected");
	assert_contains(reply.body, "0.1.0");
	assert_contains(reply.body, "8.1.3");
	assert_contains(reply.body, "libx26");
	assert_contains(reply.body, "Disk &lt;A&gt; &amp; ");
	assert_contains(reply.body, ">Proved<");
	assert_contains(reply.body, ">Not proved<");
	assert_contains(reply.body, "did not find this disk&#39;s marker file, .axis-volume, at ");
	assert_contains(reply.body, inside_b);
	assert_contains(reply.body, "every 5s");
	assert_lacks(reply.body, "The worker is attached");
	g_assert_cmpuint(axis_test_web_assert_tables(reply.body), ==, 1);
	axis_test_reply_clear(&reply);

	/* Asked about a GPU it does not have: no usable encoder of that family,
	 * what the GPU needs, and why each failed. */
	reply = get_part(fx, fx->admin, part_nvenc);
	assert_contains(reply.body, "none of the family you chose");
	assert_contains(reply.body, "No NVENC encoder passed its test encode");
	assert_contains(reply.body, "Encoders this worker measured");
	assert_lacks(reply.body, "The worker is attached");
	axis_test_reply_clear(&reply);

	/* Both disks proved: all good, the ways on, and no more asking. */
	{
		const gchar *const volumes[] = { UUID_A, inside_a, UUID_B, inside_b, NULL };

		do_register(fx, token, volumes);
	}
	{
		gint64 until = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;

		while (axis_test_stream_count(stream, "event: workers-changed") < 2 && g_get_monotonic_time() < until)
			g_main_context_iteration(NULL, FALSE);
		g_assert_cmpuint(axis_test_stream_count(stream, "event: workers-changed"), ==, 2);
	}
	reply = get_part(fx, fx->admin, part);
	assert_contains(reply.body, "The worker is attached");
	assert_contains(reply.body, "href=\"/workers#worker-sample\"");
	assert_contains(reply.body, "href=\"/conversions/runs/new\"");
	assert_lacks(reply.body, ">Not proved<");
	assert_lacks(reply.body, "every 5s");
	assert_contains(reply.body, "sse:workers-changed");
	axis_test_reply_clear(&reply);
	reply = get(fx, fx->admin, page);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "The worker is attached");
	assert_lacks(reply.body, token);
	axis_test_reply_clear(&reply);

	/* Disks asked for that are not numbers are not asked about. */
	reply = get_part(fx, fx->admin, "/workers/attach/status?name=desk&encoder=cpu&disks=1.x.<b>");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "opened without the list of disks");
	assert_lacks(reply.body, "<b>");
	axis_test_reply_clear(&reply);

	/* Revoked: said, and nothing more to wait for. */
	reply = post(fx, fx->admin, "/workers/desk/revoke", "confirm=yes");
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	reply = get_part(fx, fx->admin, part);
	assert_contains(reply.body, "This worker was revoked");
	assert_lacks(reply.body, "every 5s");
	axis_test_reply_clear(&reply);
	axis_secret_free(g_steal_pointer(&token));
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add("/web/worker-attach/who-may", Fixture, NULL, fixture_setup, test_who_may, fixture_teardown);
	g_test_add("/web/worker-attach/steps", Fixture, NULL, fixture_setup, test_steps, fixture_teardown);
	g_test_add("/web/worker-attach/each-tool", Fixture, NULL, fixture_setup, test_each_tool, fixture_teardown);
	g_test_add("/web/worker-attach/refused", Fixture, NULL, fixture_setup, test_refused, fixture_teardown);
	g_test_add("/web/worker-attach/waiting", Fixture, NULL, fixture_setup, test_waiting, fixture_teardown);

	return g_test_run();
}
