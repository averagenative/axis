/*
 * main.c - axis-worker, the program that does the encoding work the Axis
 * daemon gives it, on this machine or another
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 *   axis-worker [--config FILE]     run until SIGTERM or SIGINT
 *   axis-worker --healthcheck       0 if it was in touch with the daemon lately
 *   axis-worker --version
 *
 * There is no option for the token: it comes from a file or the
 * environment (axis-worker-config.h), never from the command line, where
 * every user of the machine could read it.
 */

#include <glib.h>
#include <locale.h>
#include <stdlib.h>

#include "axis-version.h"
#include "axis-worker-config.h"
#include "axis-worker-private.h"
#include "core/axis-log.h"

int
main(int argc, char **argv)
{
	g_autoptr(GOptionContext) context = g_option_context_new(NULL);
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisWorkerConfig) config = NULL;
	g_auto(GStrv) envp = NULL;
	g_autofree gchar *config_path = NULL;
	gboolean show_version = FALSE;
	gboolean check_health = FALSE;
	GOptionEntry entries[] = {
		{ "config", 'c', 0, G_OPTION_ARG_FILENAME, &config_path, "Read settings from FILE", "FILE" },
		{ "version", 0, 0, G_OPTION_ARG_NONE, &show_version, "Print the version and exit", NULL },
		{ "healthcheck", 0, 0, G_OPTION_ARG_NONE, &check_health,
		  "Exit 0 if the worker has been in touch with the daemon lately", NULL },
		{ NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL }
	};

	g_option_context_set_summary(context, "Axis worker: does the encoding work the Axis daemon gives it");
	g_option_context_add_main_entries(context, entries, NULL);
	if (!g_option_context_parse(context, &argc, &argv, &error)) {
		g_printerr("axis-worker: %s\n", error->message);
		return 1;
	}
	if (argc > 1) {
		g_printerr("axis-worker: unexpected argument (the token is never given on the command line: use "
			"token_file or AXIS_WORKER_TOKEN_FILE)\n");
		return 1;
	}
	if (show_version) {
		g_print("axis-worker %s (%s)\n", AXIS_VERSION, AXIS_GIT_SHA);
		return 0;
	}

	envp = g_get_environ();
	config = axis_worker_config_load(config_path, envp, !check_health, &error);
	if (config == NULL) {
		g_printerr("axis-worker: %s\n", error->message);
		return 1;
	}
	if (check_health)
		return axis_worker_work_healthcheck(config->state);

	axis_log_init(config->log_level);
	return axis_worker_run(config);
}
