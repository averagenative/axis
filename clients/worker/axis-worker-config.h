/*
 * axis-worker-config.h - axis-worker's configuration: a file and the
 * environment
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 *   [worker]
 *   url                    the daemon, http[s]://host[:port][/prefix]   (required)
 *   token_file             a file holding the worker's token            (one of these
 *   token                  the token itself (discouraged)                 two, or the
 *                                                                         environment's)
 *   ca_file                certificates to trust instead of the system's
 *   allow_insecure_http    true: plain http to an address that is not
 *                          loopback (the token then crosses the network
 *                          in clear; warned at every start)
 *   request_timeout        seconds, 1..300 (default 30)
 *   scratch                where encoders may write (default
 *                          $TMPDIR/axis-worker-<uid>); private, made if
 *                          missing, never on a volume
 *   state                  where the capability measurement and the
 *                          health file are kept (default
 *                          $TMPDIR/axis-worker-state-<uid>); private
 *   volumes                the directories where volumes are mounted here
 *                          (';'-separated in the file)
 *   volumes_without_file_identity
 *                          those of the volume paths (written as there)
 *                          that this machine sees through a network mount
 *                          that does not keep file identities: inode
 *                          numbers and sub-second times that are not the
 *                          server's (sshfs, SMB/CIFS; NFS keeps them).  A
 *                          source there is checked by its size, its time to
 *                          the second and samples of its content, not by
 *                          its inode and nanoseconds.  Default: none, every
 *                          volume checked strictly
 *   max_jobs               1..16 (default 1)
 *   encoder_families       which encoders may be run: software, nvenc,
 *                          vaapi (default all three)
 *   ffmpeg, ffprobe        the programs (absolute paths; in the file, a
 *                          ';'-separated command whose first word is the
 *                          program); default: found on PATH at start
 *   volume_check_interval  seconds between proofs of the volumes, 0.05..3600
 *                          (default 60)
 *   log_level              debug, info, warning, error
 *
 * Environment overrides, applied after the file: AXIS_WORKER_URL,
 * AXIS_WORKER_TOKEN, AXIS_WORKER_TOKEN_FILE, AXIS_WORKER_CA_FILE,
 * AXIS_WORKER_ALLOW_INSECURE_HTTP, AXIS_WORKER_REQUEST_TIMEOUT,
 * AXIS_WORKER_SCRATCH, AXIS_WORKER_STATE, AXIS_WORKER_VOLUMES
 * (','-separated), AXIS_WORKER_VOLUMES_WITHOUT_FILE_IDENTITY
 * (','-separated), AXIS_WORKER_MAX_JOBS, AXIS_WORKER_ENCODER_FAMILIES,
 * AXIS_WORKER_FFMPEG, AXIS_WORKER_FFPROBE, AXIS_WORKER_VOLUME_CHECK_INTERVAL,
 * AXIS_WORKER_LOG_LEVEL.
 *
 * The token comes from exactly one of AXIS_WORKER_TOKEN,
 * AXIS_WORKER_TOKEN_FILE, token and token_file (more than one is an
 * error, which names them).  It is never read from the command line, and
 * no message ever contains it, or anything of the configuration file's
 * text (a line the key-file reader cannot read is reported by number).
 *
 * The configuration is refused, and the worker does not start, when the
 * URL is plain http to an address that is not loopback (127.0.0.0/8, ::1,
 * localhost) and allow_insecure_http is not true.
 */

#ifndef AXIS_WORKER_CONFIG_H
#define AXIS_WORKER_CONFIG_H

#include <glib.h>

#include "core/axis-log.h"

G_BEGIN_DECLS

#define AXIS_WORKER_CONFIG_MAX_FILE_SIZE (64 * 1024)
#define AXIS_WORKER_CONFIG_MAX_VOLUMES   64

typedef struct
{
	gchar        *url;			/* base, no trailing slash */
	gboolean      https;
	gchar        *host;
	gboolean      loopback;
	gchar        *token;		/* secret: wiped when freed */
	gchar        *ca_file;
	gboolean      allow_insecure_http;
	guint         request_timeout_ms;
	gchar        *scratch;
	gchar        *state;
	gchar       **volumes;
	gchar       **no_identity;		/* of @volumes: seen through a mount without file identities */
	guint         max_jobs;
	guint         skip_families;	/* AXIS_CONVERT_FAMILY_BIT of the families not allowed */
	gchar       **ffmpeg;
	gchar       **ffprobe;
	guint         volume_check_ms;
	AxisLogLevel  log_level;
} AxisWorkerConfig;

void
axis_worker_config_free(AxisWorkerConfig *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisWorkerConfig, axis_worker_config_free)

/**
 * axis_worker_config_load:
 * @path: (nullable): the configuration file
 * @envp: the environment
 * @need_token: whether a token is required (not for --healthcheck)
 *
 * Reads, checks and completes the configuration (defaults, the programs
 * found on PATH, the scratch and state directories made private).
 *
 * Returns: (transfer full) (nullable): the configuration, or %NULL and an
 *   error that says what is wrong without quoting any secret
 */
AxisWorkerConfig *
axis_worker_config_load(
	const gchar  *path,
	gchar       **envp,
	gboolean      need_token,
	GError      **error
);

/* Whether @host names this machine's loopback (an IP literal of
 * 127.0.0.0/8 or ::1, or "localhost"). */
gboolean
axis_worker_host_is_loopback(const gchar *host);

/* A one-line summary for the log, without the token. */
gchar *
axis_worker_config_describe(const AxisWorkerConfig *self);

/* Whether the volume path @path is seen through a mount that keeps file
 * identities (it is not listed in volumes_without_file_identity). */
gboolean
axis_worker_config_keeps_identity(
	const AxisWorkerConfig *self,
	const gchar            *path
);

G_END_DECLS

#endif /* AXIS_WORKER_CONFIG_H */
