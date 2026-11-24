/*
 * test-worker-attach.c - the text "Attach a worker" hands a person: the
 * exact text for a few plans, and for every tool, encoder and kind of mount
 * what must always hold (no token, the hardening options, the disks without
 * file identities, the image's tag), and what a plan is refused for
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Pure: no database, no daemon, no container tool and no GPU.
 */

#include <glib.h>
#include <string.h>

#include "web/axis-worker-attach.h"

#define VERSION "9.8.7-rc1"
#define IMAGE   AXIS_ATTACH_IMAGE ":" VERSION

static AxisAttachPlan *
plan_new(
	AxisAttachTool    tool,
	AxisAttachEncoder encoder
){
	AxisAttachPlan *plan = axis_attach_plan_new();

	plan->name = g_strdup("desk");
	plan->tool = tool;
	plan->encoder = encoder;
	plan->url = g_strdup("https://axis.example.org");
	return plan;
}

static void
assert_has(
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

/* ── Exact text ─────────────────────────────────────────────────── */

static void
test_exact_docker(void)
{
	g_autoptr(AxisAttachPlan) plan = plan_new(AXIS_ATTACH_TOOL_DOCKER, AXIS_ATTACH_ENCODER_NVENC);
	g_autofree gchar *text = NULL;
	g_autofree gchar *file = NULL;

	axis_attach_plan_add_disk(plan, 3, "/mnt/axis/disk-3", AXIS_ATTACH_MOUNT_NFS);
	axis_attach_plan_add_disk(plan, 5, "/mnt/axis/disk-5", AXIS_ATTACH_MOUNT_SSHFS);
	text = axis_attach_generate(plan, VERSION);
	g_assert_cmpstr(text, ==,
		"docker run -d --name axis-worker-desk \\\n"
		"    --entrypoint /usr/bin/axis-worker \\\n"
		"    --user \"$(id -u):$(id -g)\" \\\n"
		"    --read-only --cap-drop ALL --security-opt no-new-privileges \\\n"
		"    --pids-limit 512 --tmpfs /tmp:rw,noexec,nosuid,size=256m,mode=1777 \\\n"
		"    --gpus all \\\n"
		"    -v \"$HOME/.config/axis-worker/desk.token:/run/secrets/axis_worker_token:ro\" \\\n"
		"    -v /mnt/axis/disk-3:/media/disk-3 \\\n"
		"    -v /mnt/axis/disk-5:/media/disk-5 \\\n"
		"    -e 'AXIS_WORKER_URL=https://axis.example.org' \\\n"
		"    -e 'AXIS_WORKER_TOKEN_FILE=/run/secrets/axis_worker_token' \\\n"
		"    -e 'AXIS_WORKER_VOLUMES=/media/disk-3,/media/disk-5' \\\n"
		"    -e 'AXIS_WORKER_VOLUMES_WITHOUT_FILE_IDENTITY=/media/disk-5' \\\n"
		"    -e 'AXIS_WORKER_ENCODER_FAMILIES=nvenc' \\\n"
		"    -e 'AXIS_WORKER_LOG_LEVEL=info' \\\n"
		"    -e 'NVIDIA_DRIVER_CAPABILITIES=compute,video,utility' \\\n"
		"    --health-cmd '/usr/bin/axis-worker --healthcheck' --health-interval 30s \\\n"
		"    --stop-timeout 75 --restart unless-stopped \\\n"
		"    " IMAGE "\n");
	file = axis_attach_file_name(plan);
	g_assert_null(file);
}

static void
test_exact_podman(void)
{
	g_autoptr(AxisAttachPlan) plan = plan_new(AXIS_ATTACH_TOOL_PODMAN, AXIS_ATTACH_ENCODER_VAAPI);
	g_autofree gchar *text = NULL;
	g_autofree gchar *start = NULL;
	g_autofree gchar *token = NULL;

	g_free(plan->url);
	plan->url = g_strdup("http://192.0.2.10:7800");
	plan->allow_http = TRUE;
	axis_attach_plan_add_disk(plan, 1, "/srv/films", AXIS_ATTACH_MOUNT_BIND);
	text = axis_attach_generate(plan, VERSION);
	g_assert_cmpstr(text, ==,
		"podman run -d --name axis-worker-desk \\\n"
		"    --entrypoint /usr/bin/axis-worker \\\n"
		"    --userns=keep-id \\\n"
		"    --read-only --cap-drop ALL --security-opt no-new-privileges \\\n"
		"    --security-opt label=disable \\\n"
		"    --pids-limit 512 --tmpfs /tmp:rw,noexec,nosuid,size=256m,mode=1777 \\\n"
		"    --device /dev/dri/renderD128 --group-add keep-groups \\\n"
		"    -v \"$HOME/.config/axis-worker/desk.token:/run/secrets/axis_worker_token:ro\" \\\n"
		"    -v /srv/films:/media/disk-1 \\\n"
		"    -e 'AXIS_WORKER_URL=http://192.0.2.10:7800' \\\n"
		"    -e 'AXIS_WORKER_ALLOW_INSECURE_HTTP=true' \\\n"
		"    -e 'AXIS_WORKER_TOKEN_FILE=/run/secrets/axis_worker_token' \\\n"
		"    -e 'AXIS_WORKER_VOLUMES=/media/disk-1' \\\n"
		"    -e 'AXIS_WORKER_ENCODER_FAMILIES=vaapi' \\\n"
		"    -e 'AXIS_WORKER_LOG_LEVEL=info' \\\n"
		"    --health-cmd '/usr/bin/axis-worker --healthcheck' --health-interval 30s \\\n"
		"    --stop-timeout 75 --restart unless-stopped \\\n"
		"    " IMAGE "\n");
	start = axis_attach_start_text(plan);
	g_assert_cmpstr(start, ==,
		"# run the command above as your own user, never as root, then:\n"
		"podman logs -f axis-worker-desk\n");
	token = axis_attach_token_text(plan);
	g_assert_cmpstr(token, ==,
		"mkdir -p ~/.config/axis-worker\n"
		"umask 077\n"
		"cat > ~/.config/axis-worker/desk.token     # paste the token, Enter, then Ctrl-D\n"
		"chmod 600 ~/.config/axis-worker/desk.token\n");
}

static void
test_exact_compose(void)
{
	g_autoptr(AxisAttachPlan) plan = plan_new(AXIS_ATTACH_TOOL_COMPOSE, AXIS_ATTACH_ENCODER_VAAPI);
	g_autofree gchar *text = NULL;
	g_autofree gchar *file = NULL;
	g_autofree gchar *start = NULL;
	g_autofree gchar *token = NULL;

	axis_attach_plan_add_disk(plan, 2, "/mnt/axis/disk-2", AXIS_ATTACH_MOUNT_SMB);
	text = axis_attach_generate(plan, VERSION);
	g_assert_cmpstr(text, ==,
		"# compose.yaml - the Axis conversion worker \"desk\"\n"
		"# Made by Axis " VERSION " (Conversion workers, Attach a worker).\n"
		"services:\n"
		"  worker:\n"
		"    image: " IMAGE "\n"
		"    container_name: axis-worker-desk\n"
		"    entrypoint: [\"/usr/bin/axis-worker\"]\n"
		"    restart: unless-stopped\n"
		"    # Your own user (PUID and PGID in .env): it reads the token file,\n"
		"    # and writes through the mounts as you do.\n"
		"    user: \"${PUID:-1000}:${PGID:-1000}\"\n"
		"    read_only: true\n"
		"    cap_drop:\n"
		"      - ALL\n"
		"    security_opt:\n"
		"      - no-new-privileges:true\n"
		"    pids_limit: 512\n"
		"    tmpfs:\n"
		"      - /tmp:rw,noexec,nosuid,size=256m,mode=1777\n"
		"    environment:\n"
		"      AXIS_WORKER_URL: \"https://axis.example.org\"\n"
		"      AXIS_WORKER_TOKEN_FILE: \"/run/secrets/axis_worker_token\"\n"
		"      AXIS_WORKER_VOLUMES: \"/media/disk-2\"\n"
		"      AXIS_WORKER_VOLUMES_WITHOUT_FILE_IDENTITY: \"/media/disk-2\"\n"
		"      AXIS_WORKER_ENCODER_FAMILIES: \"vaapi\"\n"
		"      AXIS_WORKER_LOG_LEVEL: \"info\"\n"
		"    secrets:\n"
		"      - axis_worker_token\n"
		"    volumes:\n"
		"      - /mnt/axis/disk-2:/media/disk-2\n"
		"    # The GPU's render node, and the group that owns it on this machine\n"
		"    # (RENDER_GID in .env: the output of  stat -c %g /dev/dri/renderD128).\n"
		"    devices:\n"
		"      - /dev/dri/renderD128:/dev/dri/renderD128\n"
		"    group_add:\n"
		"      - \"${RENDER_GID}\"\n"
		"    healthcheck:\n"
		"      test: [\"CMD\", \"/usr/bin/axis-worker\", \"--healthcheck\"]\n"
		"      interval: 30s\n"
		"      timeout: 5s\n"
		"      retries: 3\n"
		"      start_period: 120s\n"
		"    # A worker gives its jobs back (at most a minute), then exits.\n"
		"    stop_grace_period: 75s\n"
		"\n"
		"# The worker's token, in a file only you can read, beside this file.\n"
		"secrets:\n"
		"  axis_worker_token:\n"
		"    file: ./secrets/axis_worker_token\n");
	file = axis_attach_file_name(plan);
	g_assert_cmpstr(file, ==, "compose.yaml");
	start = axis_attach_start_text(plan);
	g_assert_cmpstr(start, ==,
		"# in the directory that holds compose.yaml and secrets/\n"
		"printf 'PUID=%s\\nPGID=%s\\n' \"$(id -u)\" \"$(id -g)\" > .env\n"
		"printf 'RENDER_GID=%s\\n' \"$(stat -c %g /dev/dri/renderD128)\" >> .env\n"
		"docker compose up -d\n"
		"docker compose logs -f worker\n");
	token = axis_attach_token_text(plan);
	g_assert_cmpstr(token, ==,
		"# in the directory where you save compose.yaml\n"
		"mkdir -p secrets\n"
		"chmod 700 secrets\n"
		"umask 077\n"
		"cat > secrets/axis_worker_token     # paste the token, Enter, then Ctrl-D\n"
		"chmod 600 secrets/axis_worker_token\n");
}

static void
test_exact_quadlet(void)
{
	g_autoptr(AxisAttachPlan) plan = plan_new(AXIS_ATTACH_TOOL_QUADLET, AXIS_ATTACH_ENCODER_CPU);
	g_autofree gchar *text = NULL;
	g_autofree gchar *file = NULL;
	g_autofree gchar *start = NULL;

	axis_attach_plan_add_disk(plan, 7, "/mnt/axis/disk-7", AXIS_ATTACH_MOUNT_NFS);
	text = axis_attach_generate(plan, VERSION);
	g_assert_cmpstr(text, ==,
		"# axis-worker-desk.container - the Axis conversion worker \"desk\"\n"
		"# Made by Axis " VERSION " (Conversion workers, Attach a worker).\n"
		"[Unit]\n"
		"Description=Axis conversion worker desk\n"
		"Wants=network-online.target\n"
		"After=network-online.target\n"
		"\n"
		"[Container]\n"
		"ContainerName=axis-worker-desk\n"
		"Image=" IMAGE "\n"
		"Entrypoint=/usr/bin/axis-worker\n"
		"# Your own user inside the container: it reads your token file, and\n"
		"# writes through the mounts as you do.\n"
		"UserNS=keep-id\n"
		"ReadOnly=true\n"
		"Tmpfs=/tmp:rw,noexec,nosuid,size=256m,mode=1777\n"
		"DropCapability=all\n"
		"NoNewPrivileges=true\n"
		"# SELinux: a network mount is refused to the container otherwise.\n"
		"SecurityLabelDisable=true\n"
		"Volume=%h/.config/axis-worker/desk.token:/run/secrets/axis_worker_token:ro\n"
		"Volume=/mnt/axis/disk-7:/media/disk-7\n"
		"Environment=AXIS_WORKER_URL=https://axis.example.org\n"
		"Environment=AXIS_WORKER_TOKEN_FILE=/run/secrets/axis_worker_token\n"
		"Environment=AXIS_WORKER_VOLUMES=/media/disk-7\n"
		"Environment=AXIS_WORKER_ENCODER_FAMILIES=software\n"
		"Environment=AXIS_WORKER_LOG_LEVEL=info\n"
		"PodmanArgs=--pids-limit=512\n"
		"HealthCmd=/usr/bin/axis-worker --healthcheck\n"
		"HealthInterval=30s\n"
		"HealthTimeout=5s\n"
		"HealthRetries=3\n"
		"HealthStartPeriod=120s\n"
		"# A worker gives its jobs back (at most a minute), then exits.\n"
		"StopTimeout=75\n"
		"\n"
		"[Service]\n"
		"Restart=always\n"
		"\n"
		"[Install]\n"
		"WantedBy=default.target\n");
	file = axis_attach_file_name(plan);
	g_assert_cmpstr(file, ==, "axis-worker-desk.container");
	start = axis_attach_start_text(plan);
	g_assert_cmpstr(start, ==,
		"mkdir -p ~/.config/containers/systemd\n"
		"# save the text above as ~/.config/containers/systemd/axis-worker-desk.container, then:\n"
		"systemctl --user daemon-reload\n"
		"systemctl --user start axis-worker-desk\n"
		"loginctl enable-linger     # once: the worker runs while you are logged out\n"
		"journalctl --user -u axis-worker-desk -f\n");
}

static void
test_exact_mounts(void)
{
	AxisAttachDisk disk;
	g_autofree gchar *bind = NULL;
	g_autofree gchar *nfs = NULL;
	g_autofree gchar *sshfs = NULL;
	g_autofree gchar *smb = NULL;

	disk.volume = 4;
	disk.host_path = (gchar *)"/mnt/axis/disk-4";
	disk.mount = AXIS_ATTACH_MOUNT_BIND;
	bind = axis_attach_mount_text(&disk);
	g_assert_cmpstr(bind, ==, "ls -a /mnt/axis/disk-4     # must show .axis-volume\n");
	disk.mount = AXIS_ATTACH_MOUNT_NFS;
	nfs = axis_attach_mount_text(&disk);
	g_assert_cmpstr(nfs, ==,
		"sudo mkdir -p /mnt/axis/disk-4\n"
		"sudo mount -t nfs4 -o rw,hard SERVER:/PATH/ON/SERVER /mnt/axis/disk-4\n"
		"ls -a /mnt/axis/disk-4     # must show .axis-volume\n");
	disk.mount = AXIS_ATTACH_MOUNT_SSHFS;
	sshfs = axis_attach_mount_text(&disk);
	g_assert_cmpstr(sshfs, ==,
		"# once, as root: let a user's FUSE mount be seen by a container\n"
		"echo user_allow_other | sudo tee -a /etc/fuse.conf\n"
		"\n"
		"sudo mkdir -p /mnt/axis/disk-4\n"
		"sudo chown \"$(id -u):$(id -g)\" /mnt/axis/disk-4\n"
		"sshfs AXIS_USER@SERVER:/PATH/ON/SERVER /mnt/axis/disk-4 \\\n"
		"    -o allow_other,reconnect,ServerAliveInterval=15,ServerAliveCountMax=3\n"
		"ls -a /mnt/axis/disk-4     # must show .axis-volume\n");
	disk.mount = AXIS_ATTACH_MOUNT_SMB;
	smb = axis_attach_mount_text(&disk);
	g_assert_cmpstr(smb, ==,
		"sudo mkdir -p /mnt/axis/disk-4\n"
		"sudo mount -t cifs //SERVER/SHARE /mnt/axis/disk-4 \\\n"
		"    -o credentials=$HOME/.config/axis-worker/smb,uid=$(id -u),gid=$(id -g),serverino\n"
		"ls -a /mnt/axis/disk-4     # must show .axis-volume\n");

	/* A path that is not in its plain form gets no text. */
	disk.host_path = (gchar *)"/mnt/axis/$(reboot)";
	g_assert_null(axis_attach_mount_text(&disk));
	disk.host_path = (gchar *)"/mnt/axis/disk-4/";
	g_assert_null(axis_attach_mount_text(&disk));
}

/* ── Every combination ──────────────────────────────────────────── */

/* The value the text gives the setting @name ("-e 'NAME=value'",
 * "NAME: \"value\"" or "Environment=NAME=value"), or NULL when the text does
 * not set it.  The setting's name must not be the end of a longer one. */
static gchar *
setting_of(
	const gchar *text,
	const gchar *name
){
	const gchar *p = text;
	gsize length = strlen(name);

	while ((p = strstr(p, name)) != NULL) {
		const gchar *after = p + length;
		gboolean starts = p == text || !(g_ascii_isalnum(p[-1]) || p[-1] == '_');
		const gchar *end;

		p = after;
		if (!starts)
			continue;
		if (after[0] == '=') {
			after++;
			end = after + strcspn(after, "'\n");
			return g_strndup(after, end - after);
		}
		if (g_str_has_prefix(after, ": \"")) {
			after += 3;
			end = after + strcspn(after, "\"\n");
			return g_strndup(after, end - after);
		}
	}
	return NULL;
}

/* No run of 43 or more characters of a token's alphabet: a token is 43 of
 * them, and nothing the text is made of is that long. */
static void
assert_no_token_shape(const gchar *text)
{
	guint run = 0;
	const gchar *p;

	for (p = text; *p != '\0'; p++) {
		run = g_ascii_isalnum(*p) || *p == '-' || *p == '_' ? run + 1 : 0;
		if (run >= 43)
			g_error("a run of 43 characters of a token's alphabet in:\n%s", text);
	}
}

static void
assert_invariants(
	const AxisAttachPlan *plan,
	const gchar          *text,
	const gchar          *all,
	const gchar          *without
){
	g_autofree gchar *volumes = NULL;
	g_autofree gchar *no_identity = NULL;
	g_autofree gchar *families = NULL;
	g_autofree gchar *token_file = NULL;
	g_autofree gchar *url = NULL;
	g_autofree gchar *insecure = NULL;

	g_assert_nonnull(text);
	g_assert_true(g_str_has_suffix(text, "\n"));

	/* The token: its file's path only. */
	assert_no_token_shape(text);
	g_assert_null(setting_of(text, "AXIS_WORKER_TOKEN"));
	token_file = setting_of(text, "AXIS_WORKER_TOKEN_FILE");
	g_assert_cmpstr(token_file, ==, "/run/secrets/axis_worker_token");
	assert_lacks(text, "--token");

	/* The image: this build's version, once. */
	g_assert_cmpuint(count_of(text, AXIS_ATTACH_IMAGE ":"), ==, 1);
	assert_has(text, IMAGE "\n");

	/* The hardening options of the example. */
	switch (plan->tool) {
	case AXIS_ATTACH_TOOL_COMPOSE:
		assert_has(text, "    read_only: true\n");
		assert_has(text, "    cap_drop:\n      - ALL\n");
		assert_has(text, "    security_opt:\n      - no-new-privileges:true\n");
		assert_has(text, "    pids_limit: 512\n");
		assert_has(text, "    tmpfs:\n      - /tmp:rw,noexec,nosuid,size=256m,mode=1777\n");
		assert_has(text, "    user: \"${PUID:-1000}:${PGID:-1000}\"\n");
		assert_lacks(text, "privileged");
		break;
	case AXIS_ATTACH_TOOL_QUADLET:
		assert_has(text, "\nReadOnly=true\n");
		assert_has(text, "\nDropCapability=all\n");
		assert_has(text, "\nNoNewPrivileges=true\n");
		assert_has(text, "\nPodmanArgs=--pids-limit=512");
		assert_has(text, "\nTmpfs=/tmp:rw,noexec,nosuid,size=256m,mode=1777\n");
		assert_has(text, "\nUserNS=keep-id\n");
		break;
	default:
		assert_has(text, " --read-only ");
		assert_has(text, " --cap-drop ALL ");
		assert_has(text, " --security-opt no-new-privileges ");
		assert_has(text, " --pids-limit 512 ");
		assert_has(text, " --tmpfs /tmp:rw,noexec,nosuid,size=256m,mode=1777 ");
		assert_has(text, plan->tool == AXIS_ATTACH_TOOL_PODMAN ? " --userns=keep-id " : " --user \"$(id -u):$(id -g)\" ");
		assert_lacks(text, "--privileged");
		break;
	}
	assert_lacks(text, "root");

	/* The disks, and of them exactly those without file identities. */
	volumes = setting_of(text, "AXIS_WORKER_VOLUMES");
	g_assert_cmpstr(volumes, ==, all);
	no_identity = setting_of(text, "AXIS_WORKER_VOLUMES_WITHOUT_FILE_IDENTITY");
	g_assert_cmpstr(no_identity, ==, without);

	/* The encoder, and a GPU only for a GPU. */
	families = setting_of(text, "AXIS_WORKER_ENCODER_FAMILIES");
	g_assert_cmpstr(families, ==, axis_attach_encoder_family(plan->encoder));
	if (plan->encoder != AXIS_ATTACH_ENCODER_NVENC) {
		assert_lacks(text, "nvidia");
		assert_lacks(text, "NVIDIA");
		assert_lacks(text, "--gpus");
	}
	if (plan->encoder != AXIS_ATTACH_ENCODER_VAAPI)
		assert_lacks(text, "/dev/dri");
	if (plan->encoder == AXIS_ATTACH_ENCODER_NVENC)
		assert_has(text, plan->tool == AXIS_ATTACH_TOOL_DOCKER ? "--gpus all" :
			plan->tool == AXIS_ATTACH_TOOL_COMPOSE ? "driver: nvidia" : "nvidia.com/gpu=all");
	if (plan->encoder == AXIS_ATTACH_ENCODER_VAAPI)
		assert_has(text, "/dev/dri/renderD128");

	url = setting_of(text, "AXIS_WORKER_URL");
	g_assert_cmpstr(url, ==, plan->url);
	insecure = setting_of(text, "AXIS_WORKER_ALLOW_INSECURE_HTTP");
	g_assert_cmpstr(insecure, ==, plan->allow_http ? "true" : NULL);
}

/* Spec: every tool, every encoder, every kind of mount, alone and mixed */
static void
test_every_combination(void)
{
	guint tool;
	guint encoder;
	guint mount;
	guint n = 0;

	for (tool = 0; tool < AXIS_ATTACH_N_TOOLS; tool++) {
		for (encoder = 0; encoder < AXIS_ATTACH_N_ENCODERS; encoder++) {
			/* One disk of each kind of mount. */
			for (mount = 0; mount < AXIS_ATTACH_N_MOUNTS; mount++) {
				g_autoptr(AxisAttachPlan) plan = plan_new((AxisAttachTool)tool, (AxisAttachEncoder)encoder);
				g_autofree gchar *text = NULL;
				gboolean keeps = axis_attach_mount_keeps_identity((AxisAttachMount)mount);

				axis_attach_plan_add_disk(plan, 11, "/mnt/axis/disk-11", (AxisAttachMount)mount);
				text = axis_attach_generate(plan, VERSION);
				assert_invariants(plan, text, "/media/disk-11", keeps ? NULL : "/media/disk-11");
				n++;
			}
			/* All four kinds at once, in an order that is not the kinds'. */
			{
				g_autoptr(AxisAttachPlan) plan = plan_new((AxisAttachTool)tool, (AxisAttachEncoder)encoder);
				g_autofree gchar *text = NULL;

				g_free(plan->url);
				plan->url = g_strdup("http://[2001:db8::10]:7800/axis");
				plan->allow_http = TRUE;
				axis_attach_plan_add_disk(plan, 4, "/mnt/d", AXIS_ATTACH_MOUNT_SMB);
				axis_attach_plan_add_disk(plan, 1, "/srv/a", AXIS_ATTACH_MOUNT_BIND);
				axis_attach_plan_add_disk(plan, 3, "/mnt/c", AXIS_ATTACH_MOUNT_SSHFS);
				axis_attach_plan_add_disk(plan, 2, "/mnt/b", AXIS_ATTACH_MOUNT_NFS);
				text = axis_attach_generate(plan, VERSION);
				assert_invariants(plan, text, "/media/disk-4,/media/disk-1,/media/disk-3,/media/disk-2",
					"/media/disk-4,/media/disk-3");
				assert_has(text, "/mnt/d:/media/disk-4");
				assert_has(text, "/srv/a:/media/disk-1");
				n++;
			}
			/* What goes around the text holds no token either. */
			{
				g_autoptr(AxisAttachPlan) plan = plan_new((AxisAttachTool)tool, (AxisAttachEncoder)encoder);
				g_autofree gchar *token = NULL;
				g_autofree gchar *start = NULL;

				axis_attach_plan_add_disk(plan, 1, "/srv/a", AXIS_ATTACH_MOUNT_BIND);
				token = axis_attach_token_text(plan);
				start = axis_attach_start_text(plan);
				g_assert_nonnull(token);
				g_assert_nonnull(start);
				assert_no_token_shape(token);
				assert_no_token_shape(start);
				assert_has(token, "chmod 600 ");
				assert_has(token, "umask 077\n");
			}
		}
	}
	g_assert_cmpuint(n, ==, 4 * 3 * 5);
}

/* ── What is refused ────────────────────────────────────────────── */

static void
assert_url(
	const gchar *typed,
	const gchar *expected,
	gboolean     plain_elsewhere
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *url = NULL;
	gboolean plain = FALSE;

	if (!axis_attach_url_normalize(typed, &url, &plain, &error))
		g_error("%s: refused: %s", typed, error->message);
	g_assert_cmpstr(url, ==, expected);
	g_assert_true(plain == plain_elsewhere);
}

static void
assert_url_refused(const gchar *typed)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *url = NULL;

	if (axis_attach_url_normalize(typed, &url, NULL, &error))
		g_error("%s: accepted as %s", typed != NULL ? typed : "(null)", url);
	g_assert_null(url);
	g_assert_nonnull(error);
	/* The sentence quotes nothing that was typed. */
	if (typed != NULL && strlen(typed) > 12)
		assert_lacks(error->message, typed);
}

static void
test_urls(void)
{
	guint i;
	static const gchar *const refused[] = {
		NULL, "", "   ", "axis.example.org", "ftp://axis.example.org", "https://", "https:///path",
		"https://user@axis.example.org", "https://user:secret@axis.example.org", "https://axis.example.org/?a=b",
		"https://axis.example.org/#top", "https://axis.example.org/a//b", "https://axis.example.org/a/../b",
		"https://axis.example.org/a/./b", "https://axis.example.org/..", "https://axis.example.org/a b",
		"https://axis.example.org/$(reboot)", "https://axis.example.org/`id`", "https://axis.example.org';reboot;'",
		"https://axis.example.org\" -e X=1", "https://axis.example.org/%2e%2e", "https://axis.example.org/a\nImage=evil",
		"https://axis.example.org\\x", "https://axis_example.org", "https://-axis.example.org", "https://axis..example.org",
		"https://axis.example.org:0", "https://axis.example.org:99999", "https://axis.example.org:port",
		"https://[fe80::1%25eth0]", "https://[::1", "https://ax\tis.example.org", "https://axis.example.org/caf\xc3\xa9",
		"https://axis.example.org,evil", "https://axis.example.org/%h", "https://axis.example.org/${HOME}"
	};
	g_autofree gchar *long_url = g_strdup_printf("https://axis.example.org/%s", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
		"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
		"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");

	assert_url("https://axis.example.org", "https://axis.example.org", FALSE);
	assert_url("  HTTPS://Axis.Example.ORG/  ", "https://axis.example.org", FALSE);
	assert_url("https://axis.example.org:8443/axis/", "https://axis.example.org:8443/axis", FALSE);
	assert_url("http://127.0.0.1:7800", "http://127.0.0.1:7800", FALSE);
	assert_url("http://localhost:7800", "http://localhost:7800", FALSE);
	assert_url("http://[::1]:7800", "http://[::1]:7800", FALSE);
	assert_url("http://192.0.2.10:7800", "http://192.0.2.10:7800", TRUE);
	assert_url("http://axis:7800", "http://axis:7800", TRUE);
	assert_url("https://[2001:db8::10]/~axis", "https://[2001:db8::10]/~axis", FALSE);
	for (i = 0; i < G_N_ELEMENTS(refused); i++)
		assert_url_refused(refused[i]);
	assert_url_refused(long_url);
}

static void
test_host_paths(void)
{
	guint i;
	static const gchar *const refused[] = {
		NULL, "", "  ", "/", "//", "mnt/disk", "./mnt", "/mnt/../etc", "/mnt/./disk", "/mnt//disk", "/mnt/my disk",
		"/mnt/disk:ro", "/mnt/disk,/etc", "/mnt/$HOME", "/mnt/`id`", "/mnt/disk;reboot", "/mnt/disk\nVolume=/:/host",
		"/mnt/disk'", "/mnt/disk\"", "/mnt/%h", "/mnt/caf\xc3\xa9", "/mnt/disk\\", "/mnt/disk*", "/mnt/~"
	};
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *again = NULL;

	g_assert_true(axis_attach_host_path_normalize("  /mnt/axis/Disk_1.b-2/  ", &path, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(path, ==, "/mnt/axis/Disk_1.b-2");
	g_assert_true(axis_attach_host_path_normalize(path, &again, NULL));
	g_assert_cmpstr(again, ==, path);
	for (i = 0; i < G_N_ELEMENTS(refused); i++) {
		g_autoptr(GError) why = NULL;
		g_autofree gchar *got = NULL;

		if (axis_attach_host_path_normalize(refused[i], &got, &why))
			g_error("%s: accepted as %s", refused[i] != NULL ? refused[i] : "(null)", got);
		g_assert_null(got);
		g_assert_nonnull(why);
	}
}

static void
assert_refused(
	AxisAttachPlan *plan,
	const gchar    *what
){
	g_autoptr(GError) error = NULL;

	if (axis_attach_plan_check(plan, &error))
		g_error("%s: the plan passed", what);
	g_assert_nonnull(error);
	g_assert_null(axis_attach_generate(plan, VERSION));
	g_assert_null(axis_attach_token_text(plan));
	g_assert_null(axis_attach_start_text(plan));
}

/* Spec: a plan that does not have the form gets no text */
static void
test_plans_refused(void)
{
	static const gchar *const names[] = {
		"", "Desk", "9lives", "desk pc", "desk;reboot", "desk\"", "desk$(id)", "-desk", "desk_pc", "d\xc3\xa9sk",
		"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
	};
	guint i;

	{
		g_autoptr(AxisAttachPlan) plan = plan_new(AXIS_ATTACH_TOOL_DOCKER, AXIS_ATTACH_ENCODER_CPU);
		g_autoptr(GError) error = NULL;
		g_autofree gchar *text = NULL;

		axis_attach_plan_add_disk(plan, 1, "/srv/a", AXIS_ATTACH_MOUNT_BIND);
		g_assert_true(axis_attach_plan_check(plan, &error));
		g_assert_no_error(error);
		text = axis_attach_generate(plan, VERSION);
		g_assert_nonnull(text);
		/* A version that is not one. */
		g_assert_null(axis_attach_generate(plan, NULL));
		g_assert_null(axis_attach_generate(plan, ""));
		g_assert_null(axis_attach_generate(plan, "1.0 --privileged"));
		g_assert_null(axis_attach_generate(plan, "1.0\nImage=evil"));
	}
	for (i = 0; i < G_N_ELEMENTS(names); i++) {
		g_autoptr(AxisAttachPlan) plan = plan_new(AXIS_ATTACH_TOOL_QUADLET, AXIS_ATTACH_ENCODER_CPU);

		axis_attach_plan_add_disk(plan, 1, "/srv/a", AXIS_ATTACH_MOUNT_BIND);
		g_free(plan->name);
		plan->name = g_strdup(names[i]);
		assert_refused(plan, names[i]);
		g_assert_null(axis_attach_container_name(names[i]));
		g_assert_null(axis_attach_file_name(plan));
	}
	{
		g_autoptr(AxisAttachPlan) plan = plan_new(AXIS_ATTACH_TOOL_COMPOSE, AXIS_ATTACH_ENCODER_CPU);

		assert_refused(plan, "no disk");
	}
	{
		g_autoptr(AxisAttachPlan) plan = plan_new(AXIS_ATTACH_TOOL_COMPOSE, AXIS_ATTACH_ENCODER_CPU);

		axis_attach_plan_add_disk(plan, 1, "/srv/a", AXIS_ATTACH_MOUNT_BIND);
		axis_attach_plan_add_disk(plan, 1, "/srv/b", AXIS_ATTACH_MOUNT_BIND);
		assert_refused(plan, "a disk twice");
	}
	{
		g_autoptr(AxisAttachPlan) plan = plan_new(AXIS_ATTACH_TOOL_COMPOSE, AXIS_ATTACH_ENCODER_CPU);

		axis_attach_plan_add_disk(plan, 1, "/srv/a", AXIS_ATTACH_MOUNT_BIND);
		axis_attach_plan_add_disk(plan, 2, "/srv/a", AXIS_ATTACH_MOUNT_NFS);
		assert_refused(plan, "a path twice");
	}
	{
		g_autoptr(AxisAttachPlan) plan = plan_new(AXIS_ATTACH_TOOL_PODMAN, AXIS_ATTACH_ENCODER_CPU);

		axis_attach_plan_add_disk(plan, 1, "/srv/a/", AXIS_ATTACH_MOUNT_BIND);
		assert_refused(plan, "a path not in its plain form");
	}
	{
		g_autoptr(AxisAttachPlan) plan = plan_new(AXIS_ATTACH_TOOL_PODMAN, AXIS_ATTACH_ENCODER_CPU);

		axis_attach_plan_add_disk(plan, 1, "/srv/a:/etc", AXIS_ATTACH_MOUNT_BIND);
		assert_refused(plan, "a path with a colon");
	}
	{
		g_autoptr(AxisAttachPlan) plan = plan_new(AXIS_ATTACH_TOOL_PODMAN, AXIS_ATTACH_ENCODER_CPU);

		axis_attach_plan_add_disk(plan, 0, "/srv/a", AXIS_ATTACH_MOUNT_BIND);
		assert_refused(plan, "a disk without a number");
	}
	{
		g_autoptr(AxisAttachPlan) plan = plan_new(AXIS_ATTACH_TOOL_PODMAN, AXIS_ATTACH_ENCODER_CPU);

		axis_attach_plan_add_disk(plan, 1, "/srv/a", AXIS_ATTACH_MOUNT_BIND);
		g_free(plan->url);
		plan->url = g_strdup("HTTPS://axis.example.org/");
		assert_refused(plan, "an address not in its plain form");
	}
	{
		g_autoptr(AxisAttachPlan) plan = plan_new(AXIS_ATTACH_TOOL_PODMAN, AXIS_ATTACH_ENCODER_CPU);
		g_autofree gchar *text = NULL;

		axis_attach_plan_add_disk(plan, 1, "/srv/a", AXIS_ATTACH_MOUNT_BIND);
		g_free(plan->url);
		plan->url = g_strdup("http://192.0.2.10:7800");
		assert_refused(plan, "plain http to another machine");
		plan->allow_http = TRUE;
		text = axis_attach_generate(plan, VERSION);
		g_assert_nonnull(text);
	}
	{
		g_autoptr(AxisAttachPlan) plan = plan_new(AXIS_ATTACH_TOOL_PODMAN, AXIS_ATTACH_ENCODER_CPU);

		for (i = 1; i <= AXIS_ATTACH_MAX_DISKS + 1; i++) {
			g_autofree gchar *path = g_strdup_printf("/mnt/d%u", i);

			axis_attach_plan_add_disk(plan, i, path, AXIS_ATTACH_MOUNT_NFS);
		}
		assert_refused(plan, "too many disks");
	}
}

static void
test_words(void)
{
	AxisAttachTool tool = AXIS_ATTACH_TOOL_DOCKER;
	AxisAttachEncoder encoder = AXIS_ATTACH_ENCODER_NVENC;
	AxisAttachMount mount = AXIS_ATTACH_MOUNT_BIND;
	AxisAttachArch arch = AXIS_ATTACH_ARCH_X86_64;
	guint i;

	for (i = 0; i < AXIS_ATTACH_N_TOOLS; i++) {
		g_assert_true(axis_attach_tool_from_word(axis_attach_tool_word((AxisAttachTool)i), &tool));
		g_assert_cmpuint(tool, ==, i);
		g_assert_cmpstr(axis_attach_tool_label((AxisAttachTool)i), !=, "");
	}
	for (i = 0; i < AXIS_ATTACH_N_ENCODERS; i++) {
		g_assert_true(axis_attach_encoder_from_word(axis_attach_encoder_word((AxisAttachEncoder)i), &encoder));
		g_assert_cmpuint(encoder, ==, i);
	}
	for (i = 0; i < AXIS_ATTACH_N_MOUNTS; i++) {
		g_assert_true(axis_attach_mount_from_word(axis_attach_mount_word((AxisAttachMount)i), &mount));
		g_assert_cmpuint(mount, ==, i);
	}
	for (i = 0; i < AXIS_ATTACH_N_ARCHS; i++) {
		g_assert_true(axis_attach_arch_from_word(axis_attach_arch_word((AxisAttachArch)i), &arch));
		g_assert_cmpuint(arch, ==, i);
	}
	g_assert_false(axis_attach_tool_from_word("kubernetes", &tool));
	g_assert_false(axis_attach_tool_from_word(NULL, &tool));
	g_assert_false(axis_attach_encoder_from_word("", &encoder));
	g_assert_false(axis_attach_mount_from_word("NFS", &mount));
	g_assert_false(axis_attach_arch_from_word("riscv", &arch));
	g_assert_true(axis_attach_mount_keeps_identity(AXIS_ATTACH_MOUNT_BIND));
	g_assert_true(axis_attach_mount_keeps_identity(AXIS_ATTACH_MOUNT_NFS));
	g_assert_false(axis_attach_mount_keeps_identity(AXIS_ATTACH_MOUNT_SSHFS));
	g_assert_false(axis_attach_mount_keeps_identity(AXIS_ATTACH_MOUNT_SMB));
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/worker/attach/exact/docker", test_exact_docker);
	g_test_add_func("/worker/attach/exact/podman", test_exact_podman);
	g_test_add_func("/worker/attach/exact/compose", test_exact_compose);
	g_test_add_func("/worker/attach/exact/quadlet", test_exact_quadlet);
	g_test_add_func("/worker/attach/exact/mounts", test_exact_mounts);
	g_test_add_func("/worker/attach/every-combination", test_every_combination);
	g_test_add_func("/worker/attach/urls", test_urls);
	g_test_add_func("/worker/attach/host-paths", test_host_paths);
	g_test_add_func("/worker/attach/plans-refused", test_plans_refused);
	g_test_add_func("/worker/attach/words", test_words);

	return g_test_run();
}
