/*
 * axis-worker-attach.c - the text "Attach a worker" hands a person
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "web/axis-worker-attach.h"

#include <gio/gio.h>
#include <string.h>

#include "api/axis-worker-proto.h"

G_DEFINE_QUARK(axis-attach-error-quark, axis_attach_error)

#define WORKER_BIN   "/usr/bin/axis-worker"
#define TMPFS        "/tmp:rw,noexec,nosuid,size=256m,mode=1777"
#define PIDS_LIMIT   "512"
#define RENDER_NODE  "/dev/dri/renderD128"
#define NVIDIA_CAPS  "compute,video,utility"
#define NVIDIA_CDI   "nvidia.com/gpu=all"
#define STOP_SECONDS "75"

/* ── The words ──────────────────────────────────────────────────── */

typedef struct
{
	const gchar *word;
	const gchar *label;
} Choice;

static const Choice tools[AXIS_ATTACH_N_TOOLS] = {
	{ "docker", "Docker (a docker run command)" },
	{ "compose", "Docker Compose (a compose.yaml)" },
	{ "podman", "Podman (a podman run command)" },
	{ "quadlet", "Podman Quadlet (a .container file for systemd)" }
};

static const Choice encoders[AXIS_ATTACH_N_ENCODERS] = {
	{ "nvenc", "NVIDIA GPU (NVENC)" },
	{ "vaapi", "Intel or AMD GPU (VA-API)" },
	{ "cpu", "CPU only (software encoders)" }
};

static const Choice mounts[AXIS_ATTACH_N_MOUNTS] = {
	{ "bind", "A disk of that machine (no network mount)" },
	{ "nfs", "NFS" },
	{ "sshfs", "sshfs (does not keep file identities)" },
	{ "smb", "SMB/CIFS (does not keep file identities)" }
};

static const Choice archs[AXIS_ATTACH_N_ARCHS] = {
	{ "x86-64", "x86-64 (Intel, AMD)" },
	{ "arm64", "ARM64 (a Raspberry Pi 4 or 5, other ARM boards)" }
};

static gboolean
choice_from_word(
	const Choice *choices,
	guint         n,
	const gchar  *word,
	guint        *index
){
	guint i;

	for (i = 0; word != NULL && i < n; i++) {
		if (strcmp(choices[i].word, word) == 0) {
			*index = i;
			return TRUE;
		}
	}
	return FALSE;
}

const gchar *
axis_attach_tool_word(AxisAttachTool tool)
{
	return (guint)tool < AXIS_ATTACH_N_TOOLS ? tools[tool].word : "";
}

const gchar *
axis_attach_tool_label(AxisAttachTool tool)
{
	return (guint)tool < AXIS_ATTACH_N_TOOLS ? tools[tool].label : "";
}

gboolean
axis_attach_tool_from_word(
	const gchar    *word,
	AxisAttachTool *tool
){
	guint index = 0;

	if (!choice_from_word(tools, AXIS_ATTACH_N_TOOLS, word, &index))
		return FALSE;
	*tool = (AxisAttachTool)index;
	return TRUE;
}

const gchar *
axis_attach_encoder_word(AxisAttachEncoder encoder)
{
	return (guint)encoder < AXIS_ATTACH_N_ENCODERS ? encoders[encoder].word : "";
}

const gchar *
axis_attach_encoder_label(AxisAttachEncoder encoder)
{
	return (guint)encoder < AXIS_ATTACH_N_ENCODERS ? encoders[encoder].label : "";
}

gboolean
axis_attach_encoder_from_word(
	const gchar       *word,
	AxisAttachEncoder *encoder
){
	guint index = 0;

	if (!choice_from_word(encoders, AXIS_ATTACH_N_ENCODERS, word, &index))
		return FALSE;
	*encoder = (AxisAttachEncoder)index;
	return TRUE;
}

const gchar *
axis_attach_encoder_family(AxisAttachEncoder encoder)
{
	switch (encoder) {
	case AXIS_ATTACH_ENCODER_NVENC:
		return "nvenc";
	case AXIS_ATTACH_ENCODER_VAAPI:
		return "vaapi";
	default:
		return "software";
	}
}

const gchar *
axis_attach_mount_word(AxisAttachMount mount)
{
	return (guint)mount < AXIS_ATTACH_N_MOUNTS ? mounts[mount].word : "";
}

const gchar *
axis_attach_mount_label(AxisAttachMount mount)
{
	return (guint)mount < AXIS_ATTACH_N_MOUNTS ? mounts[mount].label : "";
}

gboolean
axis_attach_mount_from_word(
	const gchar     *word,
	AxisAttachMount *mount
){
	guint index = 0;

	if (!choice_from_word(mounts, AXIS_ATTACH_N_MOUNTS, word, &index))
		return FALSE;
	*mount = (AxisAttachMount)index;
	return TRUE;
}

gboolean
axis_attach_mount_keeps_identity(AxisAttachMount mount)
{
	return mount != AXIS_ATTACH_MOUNT_SSHFS && mount != AXIS_ATTACH_MOUNT_SMB;
}

const gchar *
axis_attach_arch_word(AxisAttachArch arch)
{
	return (guint)arch < AXIS_ATTACH_N_ARCHS ? archs[arch].word : "";
}

const gchar *
axis_attach_arch_label(AxisAttachArch arch)
{
	return (guint)arch < AXIS_ATTACH_N_ARCHS ? archs[arch].label : "";
}

gboolean
axis_attach_arch_from_word(
	const gchar    *word,
	AxisAttachArch *arch
){
	guint index = 0;

	if (!choice_from_word(archs, AXIS_ATTACH_N_ARCHS, word, &index))
		return FALSE;
	*arch = (AxisAttachArch)index;
	return TRUE;
}

/* ── The plan ───────────────────────────────────────────────────── */

static void
disk_free(gpointer data)
{
	AxisAttachDisk *disk = data;

	g_free(disk->host_path);
	g_free(disk);
}

AxisAttachPlan *
axis_attach_plan_new(void)
{
	AxisAttachPlan *plan = g_new0(AxisAttachPlan, 1);

	plan->disks = g_ptr_array_new_with_free_func(disk_free);
	return plan;
}

void
axis_attach_plan_free(AxisAttachPlan *plan)
{
	if (plan == NULL)
		return;
	g_free(plan->name);
	g_free(plan->url);
	g_ptr_array_unref(plan->disks);
	g_free(plan);
}

void
axis_attach_plan_add_disk(
	AxisAttachPlan  *plan,
	gint64           volume,
	const gchar     *host_path,
	AxisAttachMount  mount
){
	AxisAttachDisk *disk;

	g_return_if_fail(plan != NULL);

	disk = g_new0(AxisAttachDisk, 1);
	disk->volume = volume;
	disk->host_path = g_strdup(host_path != NULL ? host_path : "");
	disk->mount = mount;
	g_ptr_array_add(plan->disks, disk);
}

/* ── The forms of what is typed ─────────────────────────────────── */

static gboolean
refuse(
	GError      **error,
	const gchar  *sentence
){
	g_set_error_literal(error, AXIS_ATTACH_ERROR, 0, sentence);
	return FALSE;
}

static gboolean
host_is_loopback(const gchar *host)
{
	g_autoptr(GInetAddress) address = NULL;

	if (g_ascii_strcasecmp(host, "localhost") == 0)
		return TRUE;
	address = g_inet_address_new_from_string(host);
	return address != NULL && g_inet_address_get_is_loopback(address);
}

/* A host name of letters, digits, '.' and '-' that starts and ends with a
 * letter or digit, or an IP address in its own notation (no zone). */
static gboolean
host_is_plain(const gchar *host)
{
	g_autoptr(GInetAddress) address = NULL;
	gsize length = strlen(host);
	const gchar *p;

	if (length == 0 || length > 253)
		return FALSE;
	if (strchr(host, ':') != NULL) {
		for (p = host; *p != '\0'; p++) {
			if (!(g_ascii_isxdigit(*p) || *p == ':' || *p == '.'))
				return FALSE;
		}
		address = g_inet_address_new_from_string(host);
		return address != NULL;
	}
	for (p = host; *p != '\0'; p++) {
		if (!(g_ascii_isalnum(*p) || *p == '.' || *p == '-'))
			return FALSE;
	}
	return g_ascii_isalnum(host[0]) && g_ascii_isalnum(host[length - 1]) && strstr(host, "..") == NULL;
}

gboolean
axis_attach_url_normalize(
	const gchar  *typed,
	gchar       **url,
	gboolean     *plain_elsewhere,
	GError      **error
){
	g_autoptr(GUri) uri = NULL;
	g_autofree gchar *text = g_strstrip(g_strdup(typed != NULL ? typed : ""));
	g_autofree gchar *path = NULL;
	g_autofree gchar *host = NULL;
	g_autofree gchar *made = NULL;
	const gchar *scheme;
	const gchar *p;
	gboolean https;
	gint port;

	g_return_val_if_fail(url != NULL, FALSE);

	*url = NULL;
	if (plain_elsewhere != NULL)
		*plain_elsewhere = FALSE;
	if (text[0] == '\0')
		return refuse(error, "Enter the address the worker reaches Axis at, such as https://axis.example.org.");
	if (strlen(text) > AXIS_ATTACH_URL_MAX)
		return refuse(error, "The address is too long.");
	for (p = text; *p != '\0'; p++) {
		/* Nothing outside printable ASCII, and none of the characters a URL
		 * of this kind never needs. */
		if (*p <= ' ' || *p > '~' || strchr("\"'`\\$%<>{}|^;&!*()#?@,=+", *p) != NULL)
			return refuse(error, "The address is not an http or https address of a plain form: a host, maybe a port, "
				"maybe a path of letters, digits and / . _ - ~, and nothing else.");
	}
	/* The path as typed: the parser below would quietly take "." and ".."
	 * parts out, and the address shown back would not be the one typed. */
	{
		const gchar *after = strstr(text, "://");
		const gchar *typed_path = after != NULL ? strchr(after + 3, '/') : NULL;

		if (typed_path != NULL && (strstr(typed_path, "//") != NULL || strstr(typed_path, "/../") != NULL ||
		    g_str_has_suffix(typed_path, "/..") || strstr(typed_path, "/./") != NULL ||
		    g_str_has_suffix(typed_path, "/.")))
			return refuse(error, "The address's path may have no empty, \".\" or \"..\" part.");
	}
	uri = g_uri_parse(text, G_URI_FLAGS_NONE, NULL);
	scheme = uri != NULL ? g_uri_get_scheme(uri) : NULL;
	if (scheme == NULL || (g_ascii_strcasecmp(scheme, "http") != 0 && g_ascii_strcasecmp(scheme, "https") != 0))
		return refuse(error, "The address must start with http:// or https://.");
	if (g_uri_get_userinfo(uri) != NULL || g_uri_get_query(uri) != NULL || g_uri_get_fragment(uri) != NULL)
		return refuse(error, "The address may have no user name, password, query or fragment.");
	if (g_uri_get_host(uri) == NULL || !host_is_plain(g_uri_get_host(uri)))
		return refuse(error, "The address needs a host: a name of letters, digits, '.' and '-', or an IP address.");
	path = g_strdup(g_uri_get_path(uri) != NULL ? g_uri_get_path(uri) : "");
	for (p = path; *p != '\0'; p++) {
		if (!(g_ascii_isalnum(*p) || *p == '/' || *p == '.' || *p == '_' || *p == '-' || *p == '~'))
			return refuse(error, "The address's path may hold only letters, digits and / . _ - ~.");
	}
	if (strstr(path, "//") != NULL || strstr(path, "/../") != NULL || g_str_has_suffix(path, "/..") ||
	    strstr(path, "/./") != NULL || g_str_has_suffix(path, "/."))
		return refuse(error, "The address's path may have no empty, \".\" or \"..\" part.");
	while (path[0] != '\0' && path[strlen(path) - 1] == '/')
		path[strlen(path) - 1] = '\0';

	https = g_ascii_strcasecmp(scheme, "https") == 0;
	host = g_ascii_strdown(g_uri_get_host(uri), -1);
	port = g_uri_get_port(uri);
	if (port == 0 || port > 65535)
		return refuse(error, "The address's port is not a port number.");
	{
		g_autofree gchar *shown = strchr(host, ':') != NULL ? g_strdup_printf("[%s]", host) : g_strdup(host);

		made = port > 0 ? g_strdup_printf("%s://%s:%d%s", https ? "https" : "http", shown, port, path) :
			g_strdup_printf("%s://%s%s", https ? "https" : "http", shown, path);
	}
	if (strlen(made) > AXIS_ATTACH_URL_MAX)
		return refuse(error, "The address is too long.");
	if (plain_elsewhere != NULL)
		*plain_elsewhere = !https && !host_is_loopback(host);
	*url = g_steal_pointer(&made);
	return TRUE;
}

gboolean
axis_attach_host_path_normalize(
	const gchar  *typed,
	gchar       **path,
	GError      **error
){
	g_autofree gchar *text = g_strstrip(g_strdup(typed != NULL ? typed : ""));
	g_auto(GStrv) parts = NULL;
	const gchar *p;
	gsize length;
	guint i;

	g_return_val_if_fail(path != NULL, FALSE);

	*path = NULL;
	length = strlen(text);
	while (length > 1 && text[length - 1] == '/')
		text[--length] = '\0';
	if (length == 0)
		return refuse(error, "Enter where each ticked disk is on the worker's machine: an absolute path, such as "
			"/mnt/axis/disk-1.");
	if (length > AXIS_ATTACH_PATH_MAX)
		return refuse(error, "A disk's path on the worker's machine is too long.");
	if (text[0] != '/' || length < 2)
		return refuse(error, "A disk's path on the worker's machine must be absolute (start with /) and not / itself.");
	for (p = text; *p != '\0'; p++) {
		if (!(g_ascii_isalnum(*p) || *p == '/' || *p == '.' || *p == '_' || *p == '-'))
			return refuse(error, "A disk's path on the worker's machine may hold only letters, digits and / . _ - "
				"here. If your path has other characters, enter a plain one and change the text by hand afterwards.");
	}
	parts = g_strsplit(text + 1, "/", -1);
	for (i = 0; parts[i] != NULL; i++) {
		if (parts[i][0] == '\0' || strcmp(parts[i], ".") == 0 || strcmp(parts[i], "..") == 0)
			return refuse(error, "A disk's path on the worker's machine may have no empty, \".\" or \"..\" part.");
	}
	*path = g_steal_pointer(&text);
	return TRUE;
}

static gboolean
version_is_plain(const gchar *version)
{
	const gchar *p;

	if (version == NULL || version[0] == '\0' || strlen(version) > AXIS_WORKER_VERSION_MAX ||
	    !g_ascii_isalnum(version[0]))
		return FALSE;
	for (p = version; *p != '\0'; p++) {
		if (!(g_ascii_isalnum(*p) || *p == '.' || *p == '_' || *p == '-'))
			return FALSE;
	}
	return TRUE;
}

gboolean
axis_attach_plan_check(
	const AxisAttachPlan  *plan,
	GError               **error
){
	g_autofree gchar *url = NULL;
	gboolean plain_elsewhere = FALSE;
	guint i;
	guint j;

	g_return_val_if_fail(plan != NULL, FALSE);

	if (!axis_worker_name_is_valid(plan->name))
		return refuse(error, "A worker's name is 1 to 48 lower-case letters, digits and '-', starting with a letter.");
	if ((guint)plan->tool >= AXIS_ATTACH_N_TOOLS || (guint)plan->encoder >= AXIS_ATTACH_N_ENCODERS ||
	    (guint)plan->arch >= AXIS_ATTACH_N_ARCHS)
		return refuse(error, "Choose the container tool, the encoder and the processor from the lists.");
	if (!axis_attach_url_normalize(plan->url, &url, &plain_elsewhere, error))
		return FALSE;
	if (strcmp(url, plan->url) != 0)
		return refuse(error, "The address is not in its plain form.");
	if (plain_elsewhere && !plan->allow_http)
		return refuse(error, "The address is plain http to another machine, and a worker refuses to send its token "
			"that way. Use an https address (Axis behind a reverse proxy with TLS), or tick the box that says the "
			"network between the two is trusted.");
	if (plan->disks->len == 0)
		return refuse(error, "Tick at least one disk: a worker is given work only on disks it can reach.");
	if (plan->disks->len > AXIS_ATTACH_MAX_DISKS)
		return refuse(error, "Too many disks are ticked for one worker.");
	for (i = 0; i < plan->disks->len; i++) {
		const AxisAttachDisk *disk = g_ptr_array_index(plan->disks, i);
		g_autofree gchar *path = NULL;

		if (disk->volume <= 0 || (guint)disk->mount >= AXIS_ATTACH_N_MOUNTS)
			return refuse(error, "Choose the disks and how each is mounted from the lists.");
		if (!axis_attach_host_path_normalize(disk->host_path, &path, error))
			return FALSE;
		if (strcmp(path, disk->host_path) != 0)
			return refuse(error, "A disk's path on the worker's machine is not in its plain form.");
		for (j = 0; j < i; j++) {
			const AxisAttachDisk *other = g_ptr_array_index(plan->disks, j);

			if (other->volume == disk->volume)
				return refuse(error, "A disk is listed twice.");
			if (strcmp(other->host_path, disk->host_path) == 0)
				return refuse(error, "Two disks have the same path on the worker's machine. Each disk is mounted at "
					"a path of its own.");
		}
	}
	return TRUE;
}

gchar *
axis_attach_container_name(const gchar *name)
{
	return axis_worker_name_is_valid(name) ? g_strdup_printf("axis-worker-%s", name) : NULL;
}

gchar *
axis_attach_container_path(gint64 volume)
{
	return g_strdup_printf("/media/disk-%" G_GINT64_FORMAT, volume);
}

/* ── The settings every tool is given ───────────────────────────── */

/* The disks' paths in the container, comma-separated: all of them, or only
 * those whose mount keeps no file identities ("" for none). */
static gchar *
volume_list(
	const AxisAttachPlan *plan,
	gboolean              only_without_identity
){
	GString *list = g_string_new(NULL);
	guint i;

	for (i = 0; i < plan->disks->len; i++) {
		const AxisAttachDisk *disk = g_ptr_array_index(plan->disks, i);
		g_autofree gchar *inside = axis_attach_container_path(disk->volume);

		if (only_without_identity && axis_attach_mount_keeps_identity(disk->mount))
			continue;
		g_string_append_printf(list, "%s%s", list->len > 0 ? "," : "", inside);
	}
	return g_string_free(list, FALSE);
}

typedef struct
{
	const gchar *name;
	gchar       *value;
} Setting;

static void
setting_add(
	GArray      *settings,
	const gchar *name,
	gchar       *value	/* taken */
){
	Setting setting;

	setting.name = name;
	setting.value = value;
	g_array_append_val(settings, setting);
}

static void
setting_clear(gpointer data)
{
	g_free(((Setting *)data)->value);
}

/* The worker's environment, in the order every text lists it.  NVIDIA's
 * variable is Docker's alone: Podman passes the GPU as a CDI device. */
static GArray *
settings_new(const AxisAttachPlan *plan)
{
	GArray *settings = g_array_new(FALSE, TRUE, sizeof(Setting));
	g_autofree gchar *without = volume_list(plan, TRUE);

	g_array_set_clear_func(settings, setting_clear);
	setting_add(settings, "AXIS_WORKER_URL", g_strdup(plan->url));
	if (plan->allow_http)
		setting_add(settings, "AXIS_WORKER_ALLOW_INSECURE_HTTP", g_strdup("true"));
	setting_add(settings, "AXIS_WORKER_TOKEN_FILE", g_strdup(AXIS_ATTACH_TOKEN_PATH));
	setting_add(settings, "AXIS_WORKER_VOLUMES", volume_list(plan, FALSE));
	if (without[0] != '\0')
		setting_add(settings, "AXIS_WORKER_VOLUMES_WITHOUT_FILE_IDENTITY", g_strdup(without));
	setting_add(settings, "AXIS_WORKER_ENCODER_FAMILIES", g_strdup(axis_attach_encoder_family(plan->encoder)));
	setting_add(settings, "AXIS_WORKER_LOG_LEVEL", g_strdup("info"));
	if (plan->encoder == AXIS_ATTACH_ENCODER_NVENC &&
	    (plan->tool == AXIS_ATTACH_TOOL_DOCKER || plan->tool == AXIS_ATTACH_TOOL_COMPOSE))
		setting_add(settings, "NVIDIA_DRIVER_CAPABILITIES", g_strdup(NVIDIA_CAPS));
	return settings;
}

/* ── docker run, podman run ─────────────────────────────────────── */

static void
run_line(
	GString     *out,
	const gchar *format,
	...
) G_GNUC_PRINTF(2, 3);

/* One more line of the command: the line before it is continued. */
static void
run_line(
	GString     *out,
	const gchar *format,
	...
){
	va_list args;

	g_string_append(out, " \\\n    ");
	va_start(args, format);
	g_string_append_vprintf(out, format, args);
	va_end(args);
}

static gchar *
generate_run(
	const AxisAttachPlan *plan,
	const gchar          *version
){
	gboolean podman = plan->tool == AXIS_ATTACH_TOOL_PODMAN;
	g_autofree gchar *container = axis_attach_container_name(plan->name);
	g_autoptr(GArray) settings = settings_new(plan);
	GString *out = g_string_new(NULL);
	guint i;

	g_string_append_printf(out, "%s run -d --name %s", podman ? "podman" : "docker", container);
	run_line(out, "--entrypoint " WORKER_BIN);
	/* The person's own user inside the container: it reads their token
	 * file, and writes through the mounts as they do. */
	if (podman)
		run_line(out, "--userns=keep-id");
	else
		run_line(out, "--user \"$(id -u):$(id -g)\"");
	run_line(out, "--read-only --cap-drop ALL --security-opt no-new-privileges");
	/* SELinux: a network mount is refused to a Podman container otherwise. */
	if (podman)
		run_line(out, "--security-opt label=disable");
	run_line(out, "--pids-limit " PIDS_LIMIT " --tmpfs " TMPFS);
	switch (plan->encoder) {
	case AXIS_ATTACH_ENCODER_NVENC:
		if (podman)
			run_line(out, "--device " NVIDIA_CDI);
		else
			run_line(out, "--gpus all");
		break;
	case AXIS_ATTACH_ENCODER_VAAPI:
		if (podman)
			run_line(out, "--device " RENDER_NODE " --group-add keep-groups");
		else
			run_line(out, "--device " RENDER_NODE " --group-add \"$(stat -c %%g " RENDER_NODE ")\"");
		break;
	default:
		break;
	}
	run_line(out, "-v \"$HOME/.config/axis-worker/%s.token:" AXIS_ATTACH_TOKEN_PATH ":ro\"", plan->name);
	for (i = 0; i < plan->disks->len; i++) {
		const AxisAttachDisk *disk = g_ptr_array_index(plan->disks, i);
		g_autofree gchar *inside = axis_attach_container_path(disk->volume);

		run_line(out, "-v %s:%s", disk->host_path, inside);
	}
	for (i = 0; i < settings->len; i++) {
		const Setting *setting = &g_array_index(settings, Setting, i);

		/* Quoted: an address may hold '[', ']' and '~'. */
		run_line(out, "-e '%s=%s'", setting->name, setting->value);
	}
	run_line(out, "--health-cmd '" WORKER_BIN " --healthcheck' --health-interval 30s");
	run_line(out, "--stop-timeout " STOP_SECONDS " --restart unless-stopped");
	run_line(out, AXIS_ATTACH_IMAGE ":%s", version);
	g_string_append_c(out, '\n');
	return g_string_free(out, FALSE);
}

/* ── compose.yaml ───────────────────────────────────────────────── */

static gchar *
generate_compose(
	const AxisAttachPlan *plan,
	const gchar          *version
){
	g_autofree gchar *container = axis_attach_container_name(plan->name);
	g_autoptr(GArray) settings = settings_new(plan);
	GString *out = g_string_new(NULL);
	guint i;

	g_string_append_printf(out,
		"# compose.yaml - the Axis conversion worker \"%s\"\n"
		"# Made by Axis %s (Conversion workers, Attach a worker).\n"
		"services:\n"
		"  worker:\n"
		"    image: " AXIS_ATTACH_IMAGE ":%s\n"
		"    container_name: %s\n"
		"    entrypoint: [\"" WORKER_BIN "\"]\n"
		"    restart: unless-stopped\n"
		"    # Your own user (PUID and PGID in .env): it reads the token file,\n"
		"    # and writes through the mounts as you do.\n"
		"    user: \"${PUID:-1000}:${PGID:-1000}\"\n"
		"    read_only: true\n"
		"    cap_drop:\n"
		"      - ALL\n"
		"    security_opt:\n"
		"      - no-new-privileges:true\n"
		"    pids_limit: " PIDS_LIMIT "\n"
		"    tmpfs:\n"
		"      - " TMPFS "\n"
		"    environment:\n",
		plan->name, version, version, container);
	for (i = 0; i < settings->len; i++) {
		const Setting *setting = &g_array_index(settings, Setting, i);

		g_string_append_printf(out, "      %s: \"%s\"\n", setting->name, setting->value);
	}
	g_string_append(out,
		"    secrets:\n"
		"      - axis_worker_token\n"
		"    volumes:\n");
	for (i = 0; i < plan->disks->len; i++) {
		const AxisAttachDisk *disk = g_ptr_array_index(plan->disks, i);
		g_autofree gchar *inside = axis_attach_container_path(disk->volume);

		g_string_append_printf(out, "      - %s:%s\n", disk->host_path, inside);
	}
	switch (plan->encoder) {
	case AXIS_ATTACH_ENCODER_NVENC:
		g_string_append(out,
			"    # The NVIDIA GPU (needs the NVIDIA Container Toolkit on this machine).\n"
			"    deploy:\n"
			"      resources:\n"
			"        reservations:\n"
			"          devices:\n"
			"            - driver: nvidia\n"
			"              count: all\n"
			"              capabilities: [gpu]\n");
		break;
	case AXIS_ATTACH_ENCODER_VAAPI:
		g_string_append(out,
			"    # The GPU's render node, and the group that owns it on this machine\n"
			"    # (RENDER_GID in .env: the output of  stat -c %g " RENDER_NODE ").\n"
			"    devices:\n"
			"      - " RENDER_NODE ":" RENDER_NODE "\n"
			"    group_add:\n"
			"      - \"${RENDER_GID}\"\n");
		break;
	default:
		break;
	}
	g_string_append(out,
		"    healthcheck:\n"
		"      test: [\"CMD\", \"" WORKER_BIN "\", \"--healthcheck\"]\n"
		"      interval: 30s\n"
		"      timeout: 5s\n"
		"      retries: 3\n"
		"      start_period: 120s\n"
		"    # A worker gives its jobs back (at most a minute), then exits.\n"
		"    stop_grace_period: " STOP_SECONDS "s\n"
		"\n"
		"# The worker's token, in a file only you can read, beside this file.\n"
		"secrets:\n"
		"  axis_worker_token:\n"
		"    file: ./secrets/axis_worker_token\n");
	return g_string_free(out, FALSE);
}

/* ── Quadlet ────────────────────────────────────────────────────── */

static gchar *
generate_quadlet(
	const AxisAttachPlan *plan,
	const gchar          *version
){
	g_autofree gchar *container = axis_attach_container_name(plan->name);
	g_autoptr(GArray) settings = settings_new(plan);
	GString *out = g_string_new(NULL);
	guint i;

	g_string_append_printf(out,
		"# %s.container - the Axis conversion worker \"%s\"\n"
		"# Made by Axis %s (Conversion workers, Attach a worker).\n"
		"[Unit]\n"
		"Description=Axis conversion worker %s\n"
		"Wants=network-online.target\n"
		"After=network-online.target\n"
		"\n"
		"[Container]\n"
		"ContainerName=%s\n"
		"Image=" AXIS_ATTACH_IMAGE ":%s\n"
		"Entrypoint=" WORKER_BIN "\n"
		"# Your own user inside the container: it reads your token file, and\n"
		"# writes through the mounts as you do.\n"
		"UserNS=keep-id\n"
		"ReadOnly=true\n"
		"Tmpfs=" TMPFS "\n"
		"DropCapability=all\n"
		"NoNewPrivileges=true\n"
		"# SELinux: a network mount is refused to the container otherwise.\n"
		"SecurityLabelDisable=true\n",
		container, plan->name, version, plan->name, container, version);
	switch (plan->encoder) {
	case AXIS_ATTACH_ENCODER_NVENC:
		g_string_append(out, "AddDevice=" NVIDIA_CDI "\n");
		break;
	case AXIS_ATTACH_ENCODER_VAAPI:
		g_string_append(out, "AddDevice=" RENDER_NODE "\n");
		break;
	default:
		break;
	}
	g_string_append_printf(out, "Volume=%%h/.config/axis-worker/%s.token:" AXIS_ATTACH_TOKEN_PATH ":ro\n",
		plan->name);
	for (i = 0; i < plan->disks->len; i++) {
		const AxisAttachDisk *disk = g_ptr_array_index(plan->disks, i);
		g_autofree gchar *inside = axis_attach_container_path(disk->volume);

		g_string_append_printf(out, "Volume=%s:%s\n", disk->host_path, inside);
	}
	for (i = 0; i < settings->len; i++) {
		const Setting *setting = &g_array_index(settings, Setting, i);

		g_string_append_printf(out, "Environment=%s=%s\n", setting->name, setting->value);
	}
	g_string_append_printf(out,
		"PodmanArgs=--pids-limit=" PIDS_LIMIT "%s\n"
		"HealthCmd=" WORKER_BIN " --healthcheck\n"
		"HealthInterval=30s\n"
		"HealthTimeout=5s\n"
		"HealthRetries=3\n"
		"HealthStartPeriod=120s\n"
		"# A worker gives its jobs back (at most a minute), then exits.\n"
		"StopTimeout=" STOP_SECONDS "\n"
		"\n"
		"[Service]\n"
		"Restart=always\n"
		"\n"
		"[Install]\n"
		"WantedBy=default.target\n",
		plan->encoder == AXIS_ATTACH_ENCODER_VAAPI ? " --group-add=keep-groups" : "");
	return g_string_free(out, FALSE);
}

gchar *
axis_attach_generate(
	const AxisAttachPlan *plan,
	const gchar          *version
){
	g_return_val_if_fail(plan != NULL, NULL);

	if (!version_is_plain(version) || !axis_attach_plan_check(plan, NULL))
		return NULL;
	switch (plan->tool) {
	case AXIS_ATTACH_TOOL_COMPOSE:
		return generate_compose(plan, version);
	case AXIS_ATTACH_TOOL_QUADLET:
		return generate_quadlet(plan, version);
	default:
		return generate_run(plan, version);
	}
}

/* ── What goes around it ────────────────────────────────────────── */

gchar *
axis_attach_file_name(const AxisAttachPlan *plan)
{
	g_return_val_if_fail(plan != NULL, NULL);

	if (!axis_worker_name_is_valid(plan->name))
		return NULL;
	switch (plan->tool) {
	case AXIS_ATTACH_TOOL_COMPOSE:
		return g_strdup("compose.yaml");
	case AXIS_ATTACH_TOOL_QUADLET:
		return g_strdup_printf("axis-worker-%s.container", plan->name);
	default:
		return NULL;
	}
}

gchar *
axis_attach_token_text(const AxisAttachPlan *plan)
{
	g_return_val_if_fail(plan != NULL, NULL);

	if (!axis_attach_plan_check(plan, NULL))
		return NULL;
	if (plan->tool == AXIS_ATTACH_TOOL_COMPOSE)
		return g_strdup(
			"# in the directory where you save compose.yaml\n"
			"mkdir -p secrets\n"
			"chmod 700 secrets\n"
			"umask 077\n"
			"cat > secrets/axis_worker_token     # paste the token, Enter, then Ctrl-D\n"
			"chmod 600 secrets/axis_worker_token\n");
	return g_strdup_printf(
		"mkdir -p ~/.config/axis-worker\n"
		"umask 077\n"
		"cat > ~/.config/axis-worker/%s.token     # paste the token, Enter, then Ctrl-D\n"
		"chmod 600 ~/.config/axis-worker/%s.token\n",
		plan->name, plan->name);
}

gchar *
axis_attach_start_text(const AxisAttachPlan *plan)
{
	g_autofree gchar *container = NULL;

	g_return_val_if_fail(plan != NULL, NULL);

	if (!axis_attach_plan_check(plan, NULL))
		return NULL;
	container = axis_attach_container_name(plan->name);
	switch (plan->tool) {
	case AXIS_ATTACH_TOOL_COMPOSE:
		return g_strdup_printf(
			"# in the directory that holds compose.yaml and secrets/\n"
			"printf 'PUID=%%s\\nPGID=%%s\\n' \"$(id -u)\" \"$(id -g)\" > .env\n"
			"%s"
			"docker compose up -d\n"
			"docker compose logs -f worker\n",
			plan->encoder == AXIS_ATTACH_ENCODER_VAAPI ?
				"printf 'RENDER_GID=%s\\n' \"$(stat -c %g " RENDER_NODE ")\" >> .env\n" : "");
	case AXIS_ATTACH_TOOL_QUADLET:
		return g_strdup_printf(
			"mkdir -p ~/.config/containers/systemd\n"
			"# save the text above as ~/.config/containers/systemd/%s.container, then:\n"
			"systemctl --user daemon-reload\n"
			"systemctl --user start %s\n"
			"loginctl enable-linger     # once: the worker runs while you are logged out\n"
			"journalctl --user -u %s -f\n",
			container, container, container);
	case AXIS_ATTACH_TOOL_PODMAN:
		return g_strdup_printf(
			"# run the command above as your own user, never as root, then:\n"
			"podman logs -f %s\n",
			container);
	default:
		return g_strdup_printf(
			"# run the command above as your own user, then:\n"
			"docker logs -f %s\n",
			container);
	}
}

gchar *
axis_attach_mount_text(const AxisAttachDisk *disk)
{
	g_autofree gchar *path = NULL;
	const gchar *host;

	g_return_val_if_fail(disk != NULL, NULL);

	if ((guint)disk->mount >= AXIS_ATTACH_N_MOUNTS || !axis_attach_host_path_normalize(disk->host_path, &path, NULL) ||
	    strcmp(path, disk->host_path) != 0)
		return NULL;
	host = disk->host_path;
	switch (disk->mount) {
	case AXIS_ATTACH_MOUNT_NFS:
		return g_strdup_printf(
			"sudo mkdir -p %s\n"
			"sudo mount -t nfs4 -o rw,hard SERVER:/PATH/ON/SERVER %s\n"
			"ls -a %s     # must show .axis-volume\n",
			host, host, host);
	case AXIS_ATTACH_MOUNT_SSHFS:
		return g_strdup_printf(
			"# once, as root: let a user's FUSE mount be seen by a container\n"
			"echo user_allow_other | sudo tee -a /etc/fuse.conf\n"
			"\n"
			"sudo mkdir -p %s\n"
			"sudo chown \"$(id -u):$(id -g)\" %s\n"
			"sshfs AXIS_USER@SERVER:/PATH/ON/SERVER %s \\\n"
			"    -o allow_other,reconnect,ServerAliveInterval=15,ServerAliveCountMax=3\n"
			"ls -a %s     # must show .axis-volume\n",
			host, host, host, host);
	case AXIS_ATTACH_MOUNT_SMB:
		return g_strdup_printf(
			"sudo mkdir -p %s\n"
			"sudo mount -t cifs //SERVER/SHARE %s \\\n"
			"    -o credentials=$HOME/.config/axis-worker/smb,uid=$(id -u),gid=$(id -g),serverino\n"
			"ls -a %s     # must show .axis-volume\n",
			host, host, host);
	default:
		return g_strdup_printf(
			"ls -a %s     # must show .axis-volume\n",
			host);
	}
}
