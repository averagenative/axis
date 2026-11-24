/*
 * main.c - axis-portal, the request portal: the pages people who are not
 * administrators use to ask the Axis daemon for a movie or a series
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 *   axis-portal [--config FILE]     serve until SIGTERM or SIGINT
 *   axis-portal --healthcheck       0 if the portal on this machine answers
 *   axis-portal --version
 *
 * There is no option for the token: it comes from a file or the environment
 * (axis-portal-config.h), never from the command line, where every user of
 * the machine could read it.  The program talks to the daemon through the
 * portal API only, holds no database connection and writes no file.
 */

#include <glib.h>
#include <glib-unix.h>
#include <htmx-glib.h>
#include <libsoup/soup.h>
#include <signal.h>
#include <string.h>

#include "axis-portal-app.h"
#include "axis-portal-config.h"
#include "axis-version.h"
#include "core/axis-log.h"

static gboolean
on_signal(gpointer user_data)
{
	g_main_loop_quit(user_data);
	return G_SOURCE_CONTINUE;
}

/* Asks the portal on this machine for its probe: 0 when it answers. */
static int
healthcheck(const AxisPortalConfig *config)
{
	g_autoptr(SoupSession) session = soup_session_new_with_options("proxy-resolver", (GProxyResolver *)NULL,
		"timeout", 5u, NULL);
	g_autofree gchar *url = g_strdup_printf("http://%s:%u/healthz",
		strchr(config->listen_host, ':') != NULL ? "[::1]" : "127.0.0.1", (guint)config->listen_port);
	g_autoptr(SoupMessage) message = soup_message_new(SOUP_METHOD_GET, url);
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) body = NULL;

	if (message == NULL)
		return 1;
	soup_message_add_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	body = soup_session_send_and_read(session, message, NULL, &error);
	if (body == NULL || soup_message_get_status(message) != 200) {
		g_printerr("axis-portal: the portal on this machine does not answer its probe\n");
		return 1;
	}
	return 0;
}

static int
run(const AxisPortalConfig *config)
{
	g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
	g_autoptr(HtmxServer) server = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *settings = axis_portal_config_describe(config);
	AxisPortalApp *app;
	guint sigint_id;
	guint sigterm_id;

	axis_log_init(config->log_level);
	axis_log_set_format(config->log_format);
	g_message("axis-portal %s (%s) starts: %s", AXIS_VERSION, AXIS_GIT_SHA, settings);
	/* (add-request-portal, slice P5 removes this line when its review has
	 * passed.) */
	g_warning("axis-portal has not been reviewed for exposure to the internet. Do not put it where the internet "
		"can reach it until the documents of Axis say that the review has passed.");
	if (config->insecure_browser_http)
		g_warning("insecure_browser_http is set: logins are served over plain http and the cookies are not Secure. "
			"This is for tests only.");
	if (!config->https && !config->loopback)
		g_warning("the daemon's address is plain http to %s, which is not this machine: the portal's token, and "
			"every password and session that passes through, cross that network unencrypted (allow_insecure_http is "
			"set)", config->host);

	app = axis_portal_app_new(config, &error);
	if (app == NULL) {
		g_printerr("axis-portal: %s\n", error->message);
		return 1;
	}
	server = htmx_server_new();
	axis_portal_app_install(app, server);
	if (!htmx_server_listen_on(server, config->listen_host, config->listen_port, &error) ||
	    !htmx_server_start(server, &error)) {
		g_printerr("axis-portal: cannot listen on %s:%u: %s\n", config->listen_host, (guint)config->listen_port,
			error->message);
		axis_portal_app_free(app);
		return 1;
	}
	g_message("listening on %s:%u", config->listen_host, (guint)config->listen_port);

	sigint_id = g_unix_signal_add(SIGINT, on_signal, loop);
	sigterm_id = g_unix_signal_add(SIGTERM, on_signal, loop);
	g_main_loop_run(loop);
	g_source_remove(sigint_id);
	g_source_remove(sigterm_id);

	g_message("stopping");
	axis_portal_app_stop(app);
	htmx_server_stop(server);
	axis_portal_app_free(app);
	return 0;
}

int
main(int argc, char **argv)
{
	g_autoptr(GOptionContext) context = g_option_context_new(NULL);
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisPortalConfig) config = NULL;
	g_auto(GStrv) envp = NULL;
	g_autofree gchar *config_path = NULL;
	gboolean show_version = FALSE;
	gboolean check_health = FALSE;
	GOptionEntry entries[] = {
		{ "config", 'c', 0, G_OPTION_ARG_FILENAME, &config_path, "Read settings from FILE", "FILE" },
		{ "version", 0, 0, G_OPTION_ARG_NONE, &show_version, "Print the version and exit", NULL },
		{ "healthcheck", 0, 0, G_OPTION_ARG_NONE, &check_health,
		  "Exit 0 if the portal on this machine answers its probe", NULL },
		{ NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL }
	};

	g_option_context_set_summary(context, "Axis request portal: the pages people use to ask Axis for a title");
	g_option_context_add_main_entries(context, entries, NULL);
	if (!g_option_context_parse(context, &argc, &argv, &error)) {
		g_printerr("axis-portal: %s\n", error->message);
		return 1;
	}
	if (argc > 1) {
		g_printerr("axis-portal: unexpected argument (the token is never given on the command line: use "
			"token_file or AXIS_PORTAL_TOKEN_FILE)\n");
		return 1;
	}
	if (show_version) {
		g_print("axis-portal %s (%s)\n", AXIS_VERSION, AXIS_GIT_SHA);
		return 0;
	}

	envp = g_get_environ();
	config = axis_portal_config_load(config_path, envp, !check_health, &error);
	if (config == NULL) {
		g_printerr("axis-portal: %s\n", error->message);
		return 1;
	}
	if (check_health)
		return healthcheck(config);
	return run(config);
}
