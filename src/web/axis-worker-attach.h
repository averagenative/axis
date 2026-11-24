/*
 * axis-worker-attach.h - what "Attach a worker" hands a person: the text
 * that starts an axis-worker container for one worker, for the container
 * tool, the encoder and the disks they chose
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Everything the wizard prints for a person to run or save comes from here
 * and from nowhere else: a `docker run` or `podman run` command, a
 * compose.yaml, or a Quadlet .container file; the commands that put the
 * token in a file; the commands that start the worker and show its log; and
 * the commands that mount a disk.  No function here is given the token, so
 * none of this text can hold it: the text names only the file the token is
 * put in.
 *
 * Nothing a person typed reaches the text as typed.  A plan is checked as a
 * whole first (axis_attach_plan_check()): the worker's name is one a worker
 * may have, the address is in the one form axis_attach_url_normalize() gives
 * and a path in the one form axis_attach_host_path_normalize() gives, both
 * made of a few plain characters that mean nothing to a shell, to YAML or to
 * systemd.  A plan that does not pass gets no text.
 *
 * Pure: no I/O, no database, no clock.
 */

#ifndef AXIS_WORKER_ATTACH_H
#define AXIS_WORKER_ATTACH_H

#include <glib.h>

G_BEGIN_DECLS

/* The published image; its tag is the version of the Axis that generates. */
#define AXIS_ATTACH_IMAGE          "ghcr.io/averagenative/axis"

/* Where the worker reads its token inside the container. */
#define AXIS_ATTACH_TOKEN_PATH     "/run/secrets/axis_worker_token"

/* The disks of one plan (a worker takes AXIS_WORKER_MAX_VOLUMES). */
#define AXIS_ATTACH_MAX_DISKS      64

/* The longest address and path a plan takes, in bytes. */
#define AXIS_ATTACH_URL_MAX        255
#define AXIS_ATTACH_PATH_MAX       255

#define AXIS_ATTACH_ERROR (axis_attach_error_quark())

GQuark
axis_attach_error_quark(void);

typedef enum
{
	AXIS_ATTACH_TOOL_DOCKER,
	AXIS_ATTACH_TOOL_COMPOSE,
	AXIS_ATTACH_TOOL_PODMAN,
	AXIS_ATTACH_TOOL_QUADLET
} AxisAttachTool;

#define AXIS_ATTACH_N_TOOLS 4

typedef enum
{
	AXIS_ATTACH_ENCODER_NVENC,	/* an NVIDIA GPU */
	AXIS_ATTACH_ENCODER_VAAPI,	/* an Intel or AMD GPU */
	AXIS_ATTACH_ENCODER_CPU		/* the software encoders */
} AxisAttachEncoder;

#define AXIS_ATTACH_N_ENCODERS 3

typedef enum
{
	AXIS_ATTACH_MOUNT_BIND,		/* a disk of the worker's own machine */
	AXIS_ATTACH_MOUNT_NFS,
	AXIS_ATTACH_MOUNT_SSHFS,	/* keeps no file identities */
	AXIS_ATTACH_MOUNT_SMB		/* keeps no file identities */
} AxisAttachMount;

#define AXIS_ATTACH_N_MOUNTS 4

/* Information only: the published image has both. */
typedef enum
{
	AXIS_ATTACH_ARCH_X86_64,
	AXIS_ATTACH_ARCH_ARM64
} AxisAttachArch;

#define AXIS_ATTACH_N_ARCHS 2

/* The word of each in a form ("docker", "compose", "podman", "quadlet";
 * "nvenc", "vaapi", "cpu"; "bind", "nfs", "sshfs", "smb"; "x86-64",
 * "arm64"), and what a page calls it. */
const gchar *axis_attach_tool_word(AxisAttachTool tool);
const gchar *axis_attach_tool_label(AxisAttachTool tool);
gboolean     axis_attach_tool_from_word(const gchar *word, AxisAttachTool *tool);

const gchar *axis_attach_encoder_word(AxisAttachEncoder encoder);
const gchar *axis_attach_encoder_label(AxisAttachEncoder encoder);
gboolean     axis_attach_encoder_from_word(const gchar *word, AxisAttachEncoder *encoder);

/* What the worker is told: "nvenc", "vaapi" or "software". */
const gchar *axis_attach_encoder_family(AxisAttachEncoder encoder);

const gchar *axis_attach_mount_word(AxisAttachMount mount);
const gchar *axis_attach_mount_label(AxisAttachMount mount);
gboolean     axis_attach_mount_from_word(const gchar *word, AxisAttachMount *mount);

/* Whether a file seen through such a mount keeps its inode number and its
 * time below a second: FALSE for sshfs and SMB. */
gboolean     axis_attach_mount_keeps_identity(AxisAttachMount mount);

const gchar *axis_attach_arch_word(AxisAttachArch arch);
const gchar *axis_attach_arch_label(AxisAttachArch arch);
gboolean     axis_attach_arch_from_word(const gchar *word, AxisAttachArch *arch);

typedef struct
{
	gint64           volume;	/* Axis's number of the disk */
	gchar           *host_path;	/* where it is on the worker's machine */
	AxisAttachMount  mount;
} AxisAttachDisk;

typedef struct
{
	gchar             *name;	/* the worker's */
	AxisAttachTool     tool;
	AxisAttachEncoder  encoder;
	AxisAttachArch     arch;
	gchar             *url;		/* where the worker reaches Axis */
	gboolean           allow_http;	/* plain http to another machine, on a trusted network */
	GPtrArray         *disks;	/* AxisAttachDisk, in the order given */
} AxisAttachPlan;

AxisAttachPlan *
axis_attach_plan_new(void);

void
axis_attach_plan_free(AxisAttachPlan *plan);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisAttachPlan, axis_attach_plan_free)

/* Adds a disk as given; axis_attach_plan_check() judges it. */
void
axis_attach_plan_add_disk(
	AxisAttachPlan  *plan,
	gint64           volume,
	const gchar     *host_path,
	AxisAttachMount  mount
);

/**
 * axis_attach_url_normalize:
 * @typed: (nullable): what was typed, or what the browser used
 * @url: (out) (transfer full): the address in its one form
 *   ("https://axis.example.org", "http://127.0.0.1:7800/axis"): scheme,
 *   host, a port if one was given, a path prefix without its last '/'
 * @plain_elsewhere: (out) (optional): plain http to a host that is not this
 *   machine, which a worker refuses unless it is told to allow it
 *
 * The rules are the worker's own (clients/worker/axis-worker-config.c): http
 * or https, a host, no user name, password, query or fragment, a path of
 * letters, digits and / . _ - ~ without empty, "." or ".." parts.  The host
 * is a name of letters, digits, '.' and '-', or an IP address.
 *
 * The error's message is a sentence for the page and quotes nothing typed.
 */
gboolean
axis_attach_url_normalize(
	const gchar  *typed,
	gchar       **url,
	gboolean     *plain_elsewhere,
	GError      **error
);

/**
 * axis_attach_host_path_normalize:
 * @typed: (nullable): what was typed
 * @path: (out) (transfer full): the path in its one form
 *
 * An absolute path of letters, digits and / . _ - with no empty, "." or
 * ".." part and no '/' at its end, at most AXIS_ATTACH_PATH_MAX bytes.  A
 * path with other characters is refused, not quoted: the person adapts the
 * text by hand.
 */
gboolean
axis_attach_host_path_normalize(
	const gchar  *typed,
	gchar       **path,
	GError      **error
);

/**
 * axis_attach_plan_check:
 *
 * Whether every part of @plan has the form the text is made from: the name
 * (axis_worker_name_is_valid()), the address and each path already in their
 * one form, 1 to AXIS_ATTACH_MAX_DISKS disks, no disk and no path twice,
 * and plain http to another machine only with @allow_http.
 */
gboolean
axis_attach_plan_check(
	const AxisAttachPlan  *plan,
	GError               **error
);

/* "axis-worker-" + the worker's name: the container's name, and the unit's.
 * %NULL for a name that is not a worker's. */
gchar *
axis_attach_container_name(const gchar *name);

/* Where the worker sees disk @volume inside its container: "/media/disk-N". */
gchar *
axis_attach_container_path(gint64 volume);

/**
 * axis_attach_generate:
 * @plan: a plan
 * @version: the version of this Axis: the image's tag
 *
 * The text for the plan's tool: a `docker run` or `podman run` command, a
 * compose.yaml, or a Quadlet .container file.  It runs the container
 * read-only, without capabilities, without new privileges, with a limit on
 * its processes and a tmpfs for /tmp; names the token's file and never a
 * token; lists the disks' paths in the container, and of them those mounted
 * with sshfs or SMB as the ones without file identities.
 *
 * Returns: (transfer full) (nullable): the text, ending in a line feed;
 *   %NULL when the plan does not pass axis_attach_plan_check() or @version
 *   is not a version
 */
gchar *
axis_attach_generate(
	const AxisAttachPlan *plan,
	const gchar          *version
);

/* The name to save the generated text under ("compose.yaml",
 * "axis-worker-NAME.container"), or %NULL for a command.  Transfer full. */
gchar *
axis_attach_file_name(const AxisAttachPlan *plan);

/* The commands that put the token in a file only the worker's user can read;
 * the token itself is pasted by the person.  %NULL for a plan that does not
 * pass.  Transfer full. */
gchar *
axis_attach_token_text(const AxisAttachPlan *plan);

/* The commands that start the worker from the generated text and show its
 * log.  %NULL for a plan that does not pass.  Transfer full. */
gchar *
axis_attach_start_text(const AxisAttachPlan *plan);

/**
 * axis_attach_mount_text:
 *
 * The commands that make one disk visible at its path on the worker's
 * machine, to adapt: the server's address, the path or share on it and the
 * user to log in as are the placeholders SERVER, /PATH/ON/SERVER, SHARE and
 * AXIS_USER, which the person replaces.  Axis prints nothing of its own
 * host.  The tool matters for sshfs only: Docker's daemon is root and needs
 * allow_other; rootless Podman mounts and runs as the same user.  %NULL when
 * the disk's path is not in its one form or the tool is not one.  Transfer
 * full.
 */
gchar *
axis_attach_mount_text(
	const AxisAttachDisk *disk,
	AxisAttachTool        tool
);

G_END_DECLS

#endif /* AXIS_WORKER_ATTACH_H */
