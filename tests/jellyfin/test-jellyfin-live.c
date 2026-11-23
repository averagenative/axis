/*
 * test-jellyfin-live.c - the daemon's collection against a real Jellyfin
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Not part of `make test`: run by `make check-jellyfin`, which starts a
 * Jellyfin server in a container (tests/jellyfin/up.sh), makes nine clients
 * play on it (play.sh) and then runs this program with AXIS_TEST_JELLYFIN_URL
 * and AXIS_TEST_JELLYFIN_KEY set.  A second check (notify) writes a new film
 * under the server's library, tells the server its folder with the daemon's
 * own notification code (the one pinned POST, sent directly, not through
 * the proxy below) and waits for the server to list it.
 *
 * The daemon's code is pointed not at the server but at a recording proxy on
 * loopback that forwards what it receives to the server, so that what was
 * *sent* can be asserted too: only GET requests, only to the paths the
 * collection needs, the key only in the Authorization header and never in a
 * URL.  The proxy refuses anything but GET, so a stray request could not
 * change the server even if the daemon made one.
 *
 * Asserted: the connection test, one poll recording the nine sessions with
 * the evidence the server's own reasons give (and the set of reasons seen),
 * a second poll that records nothing new, the libraries and the devices, and
 * that the key is in no table and no log line.
 */

#include <glib.h>
#include <libsoup/soup.h>
#include <stdlib.h>
#include <string.h>

#include "axis-test-compat.h"
#include "axis-test-jobs.h"
#include "compat/axis-compat-notify.h"
#include "core/axis-http.h"

typedef struct
{
	GThread      *thread;
	GMainContext *context;
	GMainLoop    *loop;
	SoupServer   *server;
	SoupSession  *upstream;
	GMutex        lock;
	GCond         ready;
	gboolean      started;
	guint16       port;
	gchar        *upstream_base;
	GPtrArray    *methods;	/* (lock) */
	GPtrArray    *targets;	/* (lock) */
	GPtrArray    *auths;	/* (lock) */
} Proxy;

static void
proxy_handle(
	SoupServer        *server,
	SoupServerMessage *message,
	const char        *path,
	GHashTable        *query,
	gpointer           user_data
){
	Proxy *proxy = user_data;
	const gchar *method = soup_server_message_get_method(message);
	GUri *uri = soup_server_message_get_uri(message);
	const gchar *q = g_uri_get_query(uri);
	g_autofree gchar *target = q != NULL ? g_strconcat(path, "?", q, NULL) : g_strdup(path);
	const gchar *auth = soup_message_headers_get_one(soup_server_message_get_request_headers(message), "Authorization");
	g_autofree gchar *url = NULL;
	g_autoptr(SoupMessage) up = NULL;
	g_autoptr(GBytes) bytes = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *type;
	const gchar *data;
	gsize length;

	g_mutex_lock(&proxy->lock);
	g_ptr_array_add(proxy->methods, g_strdup(method));
	g_ptr_array_add(proxy->targets, g_strdup(target));
	g_ptr_array_add(proxy->auths, g_strdup(auth != NULL ? auth : ""));
	g_mutex_unlock(&proxy->lock);

	/* Nothing but a read reaches the real server. */
	if (strcmp(method, "GET") != 0) {
		soup_server_message_set_status(message, 405, NULL);
		return;
	}
	url = g_strconcat(proxy->upstream_base, target, NULL);
	up = soup_message_new(SOUP_METHOD_GET, url);
	if (auth != NULL)
		soup_message_headers_replace(soup_message_get_request_headers(up), "Authorization", auth);
	bytes = soup_session_send_and_read(proxy->upstream, up, NULL, &error);
	if (bytes == NULL) {
		soup_server_message_set_status(message, 502, NULL);
		return;
	}
	type = soup_message_headers_get_one(soup_message_get_response_headers(up), "Content-Type");
	data = g_bytes_get_data(bytes, &length);
	soup_server_message_set_status(message, soup_message_get_status(up), NULL);
	soup_server_message_set_response(message, type != NULL ? type : "application/json", SOUP_MEMORY_COPY, data, length);
}

static gpointer
proxy_thread(gpointer data)
{
	Proxy *proxy = data;
	g_autoptr(GError) error = NULL;
	GSList *uris;

	g_main_context_push_thread_default(proxy->context);
	proxy->server = soup_server_new(NULL, NULL);
	proxy->upstream = soup_session_new_with_options("proxy-resolver", (GProxyResolver *)NULL, NULL);
	soup_server_add_handler(proxy->server, NULL, proxy_handle, proxy, NULL);
	if (!soup_server_listen_local(proxy->server, 0, 0, &error))
		g_error("the proxy cannot listen: %s", error->message);
	uris = soup_server_get_uris(proxy->server);
	proxy->port = (guint16)g_uri_get_port(uris->data);
	g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
	g_mutex_lock(&proxy->lock);
	proxy->started = TRUE;
	g_cond_broadcast(&proxy->ready);
	g_mutex_unlock(&proxy->lock);
	g_main_loop_run(proxy->loop);
	soup_server_disconnect(proxy->server);
	g_clear_object(&proxy->server);
	g_clear_object(&proxy->upstream);
	while (g_main_context_iteration(proxy->context, FALSE))
		;
	g_main_context_pop_thread_default(proxy->context);
	return NULL;
}

static Proxy *
proxy_new(const gchar *upstream_base)
{
	Proxy *proxy = g_new0(Proxy, 1);

	g_mutex_init(&proxy->lock);
	g_cond_init(&proxy->ready);
	proxy->upstream_base = g_strdup(upstream_base);
	proxy->methods = g_ptr_array_new_with_free_func(g_free);
	proxy->targets = g_ptr_array_new_with_free_func(g_free);
	proxy->auths = g_ptr_array_new_with_free_func(g_free);
	proxy->context = g_main_context_new();
	proxy->loop = g_main_loop_new(proxy->context, FALSE);
	proxy->thread = g_thread_new("jellyfin-proxy", proxy_thread, proxy);
	g_mutex_lock(&proxy->lock);
	while (!proxy->started)
		g_cond_wait(&proxy->ready, &proxy->lock);
	g_mutex_unlock(&proxy->lock);
	return proxy;
}

static void
proxy_free(Proxy *proxy)
{
	g_main_loop_quit(proxy->loop);
	g_main_context_wakeup(proxy->context);
	g_thread_join(proxy->thread);
	g_main_loop_unref(proxy->loop);
	g_main_context_unref(proxy->context);
	g_ptr_array_unref(proxy->methods);
	g_ptr_array_unref(proxy->targets);
	g_ptr_array_unref(proxy->auths);
	g_free(proxy->upstream_base);
	g_mutex_clear(&proxy->lock);
	g_cond_clear(&proxy->ready);
	g_free(proxy);
}

/* ── The check ──────────────────────────────────────────────────── */

static gint64
count(
	AxisTestCompat *t,
	const gchar    *table,
	const gchar    *where
){
	return axis_test_compat_count(t, table, where);
}

#define EVIDENCE "compat_evidence e JOIN compat_session s ON s.id = e.session_id"

static void
test_the_daemons_collection_against_a_real_jellyfin(void)
{
	const gchar *url = g_getenv("AXIS_TEST_JELLYFIN_URL");
	const gchar *key = g_getenv("AXIS_TEST_JELLYFIN_KEY");
	AxisTestCompat *t;
	Proxy *proxy;
	g_autofree gchar *base = NULL;
	g_autofree gchar *seen = NULL;
	g_autofree gchar *authorization = NULL;
	AxisCompatTestResult result;
	AxisCompatPollSummary summary;
	g_autoptr(GError) error = NULL;
	guint i;

	if (url == NULL || key == NULL)
		g_error("AXIS_TEST_JELLYFIN_URL and AXIS_TEST_JELLYFIN_KEY are not set: run `make check-jellyfin`");
	axis_test_log_init();
	axis_test_log_reset();
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*is offline at*");
	t = axis_test_compat_new(FALSE);
	if (t == NULL)
		return;
	proxy = proxy_new(url);
	base = g_strdup_printf("http://127.0.0.1:%u", proxy->port);
	g_assert_true(axis_settings_set(t->settings, AXIS_COMPAT_JELLYFIN_ADDRESS, base, &error));
	g_assert_true(axis_settings_set(t->settings, AXIS_COMPAT_JELLYFIN_TOKEN, key, &error));
	g_assert_no_error(error);

	/* The connection test names the server and its version. */
	axis_compat_test_connection_kind(t->compat, AXIS_COMPAT_KIND_JELLYFIN, NULL, &result);
	g_assert_true(result.ok);
	g_assert_nonnull(result.name);
	g_assert_true(g_str_has_prefix(result.version, "10."));
	g_message("live check: the server is %s %s", result.name, result.version);
	axis_compat_test_result_clear(&result);

	/* A wrong key is refused, in words, without the key. */
	g_assert_true(axis_settings_set(t->settings, AXIS_COMPAT_JELLYFIN_TOKEN, "0123456789abcdef0123456789abcdef", &error));
	axis_compat_test_connection_kind(t->compat, AXIS_COMPAT_KIND_JELLYFIN, NULL, &result);
	g_assert_false(result.ok);
	g_assert_nonnull(strstr(result.message, "token was refused"));
	g_assert_null(strstr(result.message, "0123456789abcdef0123456789abcdef"));
	axis_compat_test_result_clear(&result);
	g_assert_true(axis_settings_set(t->settings, AXIS_COMPAT_JELLYFIN_TOKEN, key, &error));

	/* One look at the sessions: nine clients play, a tenth plays nothing. */
	g_assert_true(axis_compat_poll(t->compat, TRUE, NULL, &summary, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(summary.playing, ==, 9);
	g_assert_cmpuint(summary.recorded, ==, 9);
	g_assert_cmpuint(summary.skipped, ==, 0);
	g_assert_cmpint(count(t, "compat_session", "server_kind = 'jellyfin'"), ==, 9);
	g_assert_cmpint(count(t, "compat_library", "server_kind = 'jellyfin'"), ==, 2);
	g_assert_cmpint(count(t, "compat_device", "server_kind = 'jellyfin'"), >=, 10);
	g_assert_cmpint(count(t, "compat_session", "library_key IS NOT NULL"), ==, 9);
	g_assert_cmpint(count(t, "compat_state", "server_kind = 'jellyfin' AND last_poll_ok_at IS NOT NULL AND"
		" server_version LIKE '10.%'"), ==, 1);

	/* What each client's server-stated reasons made of it. */
	g_assert_cmpint(count(t, EVIDENCE, "s.device_id = 'axis-test-tv' AND e.effect = 'direct' AND e.container = 'mkv'"), ==, 2);
	g_assert_cmpint(count(t, EVIDENCE, "s.device_id = 'axis-test-speaker' AND e.effect = 'direct' AND e.codec = 'flac'"), ==, 1);
	g_assert_cmpint(count(t, EVIDENCE, "s.device_id = 'axis-test-phone' AND e.kind = 1 AND e.failed_property = ''"
		" AND e.effect = 'converted_codec' AND e.codec = 'hevc' AND e.stated_reason = 'VideoCodecNotSupported'"), ==, 1);
	g_assert_cmpint(count(t, EVIDENCE, "s.device_id = 'axis-test-phone' AND e.failed_property = 'container'"
		" AND e.container = 'mkv' AND e.stated_reason = 'ContainerNotSupported'"), ==, 1);
	g_assert_cmpint(count(t, EVIDENCE, "s.device_id = 'axis-test-stick' AND e.kind = 1 AND e.failed_property = ''"
		" AND e.effect = 'passthrough'"), ==, 1);
	g_assert_cmpint(count(t, EVIDENCE, "s.device_id = 'axis-test-stick' AND e.failed_property = 'container'"), ==, 1);
	g_assert_cmpint(count(t, EVIDENCE, "s.device_id = 'axis-test-tablet' AND e.failed_property = 'bit_depth'"
		" AND e.stated_reason = 'VideoBitDepthNotSupported'"), ==, 1);
	g_assert_cmpint(count(t, EVIDENCE, "s.device_id = 'axis-test-tablet' AND e.kind = 1 AND e.failed_property = ''"), ==, 0);
	g_assert_cmpint(count(t, EVIDENCE, "s.device_id = 'axis-test-projector' AND e.failed_property = 'level'"
		" AND e.stated_reason = 'VideoLevelNotSupported'"), ==, 1);
	g_assert_cmpint(count(t, EVIDENCE, "s.device_id = 'axis-test-hotel' AND e.effect = 'converted_limit'"
		" AND e.stated_reason = 'ContainerBitrateExceedsLimit'"), >=, 1);
	g_assert_cmpint(count(t, EVIDENCE, "s.device_id = 'axis-test-hotel' AND e.effect = 'converted_codec'"), ==, 0);
	g_assert_cmpint(count(t, EVIDENCE, "s.device_id = 'axis-test-console' AND e.kind = 2 AND e.codec = 'ac3'"
		" AND e.effect = 'converted_codec' AND e.stated_reason = 'AudioCodecNotSupported'"), ==, 1);
	g_assert_cmpint(count(t, EVIDENCE, "s.device_id = 'axis-test-browser' AND e.kind = 3"
		" AND e.failed_property = 'subtitle_format' AND e.stated_reason = 'SubtitleCodecNotSupported'"), ==, 1);

	/* The reasons the server really stated, all of them. */
	seen = axis_test_db_query(t->lib->db, "SELECT string_agg(DISTINCT r, ',' ORDER BY r) FROM (SELECT"
		" unnest(string_to_array(stated_reason, ',')) AS r FROM compat_evidence WHERE stated_reason IS NOT NULL) x");
	g_message("live check: reasons seen: %s", seen);
	g_assert_cmpstr(seen, ==, "AudioCodecNotSupported,ContainerBitrateExceedsLimit,ContainerNotSupported,"
		"SubtitleCodecNotSupported,VideoBitDepthNotSupported,VideoCodecNotSupported,VideoLevelNotSupported");
	/* A conversion blamed on a property never blames the codec as well. */
	g_assert_cmpint(count(t, "compat_evidence", "failed_property <> '' AND effect <> 'converted_codec'"), ==, 0);

	/* Looked at again: nothing new. */
	g_assert_true(axis_compat_poll(t->compat, TRUE, NULL, &summary, &error));
	g_assert_cmpuint(summary.recorded, ==, 0);
	g_assert_cmpuint(summary.updated, ==, 9);
	g_assert_cmpint(count(t, "compat_session", NULL), ==, 9);

	/* What was sent: reads only, of the paths the collection needs, with the
	 * key in the header and nowhere else. */
	g_mutex_lock(&proxy->lock);
	/* Two tests of the connection, the first look (the identity, the libraries,
	 * the devices, the sessions) and the second (the identity, the sessions). */
	g_assert_cmpuint(proxy->methods->len, ==, 8);
	for (i = 0; i < proxy->methods->len; i++) {
		const gchar *target = g_ptr_array_index(proxy->targets, i);

		g_assert_cmpstr(g_ptr_array_index(proxy->methods, i), ==, "GET");
		g_assert_true(strcmp(target, "/System/Info") == 0 || strcmp(target, "/Library/VirtualFolders") == 0 ||
			strcmp(target, "/Devices") == 0 || strcmp(target, "/Sessions") == 0 ||
			g_str_has_prefix(target, "/Items?Ids="));
		g_assert_null(strstr(target, key));
		g_assert_null(strstr(target, "api_key"));
	}
	g_mutex_unlock(&proxy->lock);
	authorization = g_strdup_printf("MediaBrowser Token=\"%s\"", key);
	g_mutex_lock(&proxy->lock);
	for (i = 0; i < proxy->auths->len; i++) {
		const gchar *auth = g_ptr_array_index(proxy->auths, i);

		/* The wrong key of the connection test is the one other value. */
		g_assert_true(strcmp(auth, authorization) == 0 ||
			strcmp(auth, "MediaBrowser Token=\"0123456789abcdef0123456789abcdef\"") == 0);
	}
	g_mutex_unlock(&proxy->lock);

	/* The key is in no table and no log line. */
	{
		static const gchar *const tables[] = {
			"job", "compat_state", "compat_session", "compat_device", "compat_library", "compat_play",
			"compat_path_map", "compat_evidence", NULL
		};
		g_autofree gchar *pattern = g_strdup_printf("*%s*", key);

		for (i = 0; tables[i] != NULL; i++) {
			g_autofree gchar *table = g_strconcat(tables[i], " x", NULL);
			g_autofree gchar *where = g_strdup_printf("to_jsonb(x)::text LIKE '%%%s%%'", key);

			g_assert_cmpint(count(t, table, where), ==, 0);
		}
		g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, pattern), ==, 0);
		g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_WARNING, pattern), ==, 0);
	}

	proxy_free(proxy);
	axis_test_compat_free(t);
	axis_test_log_reset();
}

/* The names of the movies the server lists, as its answer says (a plain
 * search of the text is enough for the one name asked for). */
static gchar *
list_movies(
	const gchar *url,
	const gchar *key
){
	AxisHttpOrigin origin;
	AxisHttpConfig config;
	g_autofree gchar *problem = NULL;
	g_autofree gchar *auth = g_strdup_printf("MediaBrowser Token=\"%s\"", key);
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GError) error = NULL;
	gchar *text;
	const gchar *data;
	gsize length = 0;

	g_assert_true(axis_http_origin_parse(url, &origin, &problem));
	memset(&config, 0, sizeof(config));
	config.origin = &origin;
	config.auth_header = "Authorization";
	config.auth_value = auth;
	config.token = key;
	config.timeout_ms = 20000;
	g_assert_true(axis_http_get(&config, "/Items?Recursive=true&IncludeItemTypes=Movie&Fields=Path", NULL, &body,
		&error));
	g_assert_no_error(error);
	data = g_bytes_get_data(body, &length);
	text = g_strndup(data, length);
	/* The three films of the server, so that "nothing listed" cannot pass. */
	g_assert_nonnull(strstr(text, "Test Film H264"));
	axis_http_origin_clear(&origin);
	return text;
}

/* After a notification the real Jellyfin lists a file that was not there:
 * a film's folder is made under its library on the host, the daemon's own
 * notify code is run for that folder (path map and all), and the server,
 * which has its watcher off and is not asked to scan the library, lists the
 * film within a couple of minutes.  Before the notification it does not. */
static void
test_a_notification_makes_jellyfin_list_a_new_file(void)
{
	const gchar *url = g_getenv("AXIS_TEST_JELLYFIN_URL");
	const gchar *key = g_getenv("AXIS_TEST_JELLYFIN_KEY");
	const gchar *media = g_getenv("AXIS_TEST_JELLYFIN_MEDIA");
	AxisTestCompat *t;
	g_autofree gchar *source = NULL;
	g_autofree gchar *folder = NULL;
	g_autofree gchar *file = NULL;
	g_autofree gchar *films = NULL;
	g_autofree gchar *problem = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *contents = NULL;
	g_autoptr(GError) error = NULL;
	AxisCompatNotifySummary summary;
	gboolean listed = FALSE;
	gsize length = 0;
	guint i;

	if (url == NULL || key == NULL || media == NULL)
		g_error("AXIS_TEST_JELLYFIN_URL, _KEY and _MEDIA are not set: run `make check-jellyfin`");
	axis_test_log_init();
	axis_test_log_reset();
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*is offline at*");
	t = axis_test_compat_new(FALSE);
	if (t == NULL)
		return;
	g_assert_true(axis_settings_set(t->settings, AXIS_COMPAT_JELLYFIN_ADDRESS, url, &error));
	g_assert_true(axis_settings_set(t->settings, AXIS_COMPAT_JELLYFIN_TOKEN, key, &error));
	g_assert_no_error(error);
	/* The server sees its library at /media/films; here it is under the
	 * host's directory. */
	films = g_build_filename(media, "films", NULL);
	g_assert_true(axis_compat_path_map_add_kind(t->compat, AXIS_COMPAT_KIND_JELLYFIN, "/media/films", films, &problem,
		&error));
	g_assert_no_error(error);

	before = list_movies(url, key);
	g_assert_null(strstr(before, "Test Film New"));

	/* A new film, a copy of one it has, in the folder named as Jellyfin
	 * wants. */
	source = g_build_filename(films, "Test Film H264 (2024)", "Test Film H264 (2024).mkv", NULL);
	g_assert_true(g_file_get_contents(source, &contents, &length, &error));
	folder = g_build_filename(films, "Test Film New (2025)", NULL);
	g_assert_cmpint(g_mkdir_with_parents(folder, 0755), ==, 0);
	file = g_build_filename(folder, "Test Film New (2025).mkv", NULL);
	g_assert_true(g_file_set_contents(file, contents, (gssize)length, &error));
	g_assert_no_error(error);
	/* Not found by itself. */
	{
		g_autofree gchar *now = list_movies(url, key);

		g_assert_null(strstr(now, "Test Film New"));
	}

	/* Told, as an import tells it. */
	g_assert_true(axis_compat_notify_record(t->lib->pool, t->settings, folder, AXIS_COMPAT_CHANGE_CREATED, &error));
	g_assert_no_error(error);
	g_assert_true(axis_compat_notify_run(t->compat, TRUE, NULL, &summary, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(summary.sent, ==, 1);
	g_assert_cmpuint(summary.failed, ==, 0);
	g_assert_cmpint(count(t, "compat_notify", "state = 'sent' AND sent_folder = '/media/films/Test Film New (2025)'"), ==, 1);

	for (i = 0; i < 90 && !listed; i++) {
		g_autofree gchar *now = list_movies(url, key);

		listed = strstr(now, "Test Film New") != NULL;
		if (!listed)
			g_usleep(2 * G_USEC_PER_SEC);
	}
	if (!listed) {
		g_autofree gchar *now = list_movies(url, key);

		g_message("live check: the movies listed at the end: %.1500s", now);
	}
	g_assert_true(listed);
	g_message("live check: Jellyfin listed the new film after being told its folder");
	axis_test_compat_free(t);
	axis_test_log_reset();
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/jellyfin-live/collection", test_the_daemons_collection_against_a_real_jellyfin);
	g_test_add_func("/jellyfin-live/notify", test_a_notification_makes_jellyfin_list_a_new_file);
	return g_test_run();
}
