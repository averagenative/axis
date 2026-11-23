/*
 * axis-conversion-verify.c - the daemon's ten checks of a converted file
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Nothing here takes a worker's word for a verdict: the output is opened in
 * the work area by the name the daemon chose, the plan is built again from
 * the daemon's own records, and every check is decided here.  Checks 1 to
 * 4, 9 and 10, check 6's durations and check 7's attachments are measured
 * here, by the daemon's own ffprobe and ffmpeg, each run with the file as a
 * descriptor (never a name), only the fd protocol and the probe's container
 * whitelist, under the probe runner's limits and a time limit.  The
 * measurements of checks 5 to 8 are made by transcode/axis-verify-measure.c,
 * here with convert.check_threads or, where convert.checks_where sends them
 * to a worker, by axis-worker with the same code; a worker's are used only
 * once they are bound to the files on this disk (the output's stamp at
 * hand-out, samples of both files, by default the whole output's SHA-256),
 * and both are judged by the same functions (axis-verify-judge.c).  The
 * checks run in order and stop at the first that fails (3 and 4 always run
 * together); each is recorded with what it found.  A check that cannot be
 * made fails: nothing passes by default.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "transcode/axis-conversion-private.h"

#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <math.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "transcode/axis-estimate-private.h"

#include "library/axis-library-name.h"
#include "probe/axis-probe-describe.h"
#include "probe/axis-probe-run.h"
#include "storage/axis-file-check.h"
#include "storage/axis-storage-open.h"
#include "transcode/axis-convert-caps.h"

#define ERROR_CAP        (16 * 1024)
#define MEMORY_LIMIT     (G_GUINT64_CONSTANT(8) * 1024 * 1024 * 1024)
#define POINTS           5

#ifndef IOPRIO_WHO_PROCESS
#define IOPRIO_WHO_PROCESS 1
#endif
#define IOPRIO_CLASS_IDLE_VALUE (3 << 13)

typedef struct
{
	AxisConversions      *self;
	AxisConversionRow    *row;
	AxisVolumeGuard      *guard;
	gint                  work;
	AxisJobContext       *context;
	AxisConversionFacts   facts;
	gchar               **ffmpeg;
	gchar               **ffprobe;
	gchar                *scratch;
	gint                  out_fd;
	gint                  src_fd;
	AxisStorageOpenStatus src_status;	/* why the original could not be opened */
	struct stat           out_st;
	struct stat           src_st;
	AxisConvertPlan      *plan;
	AxisMediaInfo        *output;
	guint                 timeout_ms;
	guint64               picture_bytes;
	gdouble               picture_seconds;
	AxisConversionCheckMode    mode;
	const AxisVerifyEvidence  *given;	/* JUDGE: a worker's, bound to the files */
	AxisVerifyEvidence        *measured;	/* ALL: this daemon's */
	AxisVerifyPlan            *checks;	/* what 5 to 8 measure, made after check 3 */
	guint                      threads;	/* convert.check_threads */
	gchar                     *not_judged;	/* check 3: the plan cannot be made as it was given */
	gchar                     *output_sample;	/* the daemon's digest of samples of the output */
	gchar                     *output_digest;	/* JUDGE, confirmed whole: its SHA-256 */
} Verify;

/* The measurements checks 5 to 8 are judged on. */
static const AxisVerifyEvidence *
evidence_of(Verify *v)
{
	return v->mode == AXIS_CONVERSION_CHECK_JUDGE ? v->given : v->measured;
}

/* The tools as this daemon runs them for 5 to 8: bounded threads. */
static void
tools_of(
	Verify          *v,
	AxisVerifyTools *tools
);

static GCancellable *
cancellable_of(Verify *v)
{
	return v->context != NULL ? axis_job_context_get_cancellable(v->context) : NULL;
}

static gboolean
stopped(Verify *v)
{
	return v->context != NULL && axis_job_context_is_cancelled(v->context);
}

static gboolean
record(
	Verify       *v,
	guint         number,
	gboolean      passed,
	const gchar  *detail,
	GError      **error
){
	return axis_conversion_record_check_pool(v->self->pool, v->row->id, number, passed ? "passed" : "failed", detail,
		error);
}

/* The facts as they are now (a connection only while they are read). */
static gboolean
load_facts(
	Verify  *v,
	GError **error
){
	AxisDbConn *conn = axis_db_pool_acquire(v->self->pool, error);
	gboolean ok;

	if (conn == NULL)
		return FALSE;
	axis_conversion_facts_clear(&v->facts);
	ok = axis_conversion_facts_load(v->self, conn, v->row->file_id, v->row->revision_id, v->row->id, &v->facts,
		error);
	axis_db_pool_release(v->self->pool, conn);
	return ok;
}

static GPtrArray *
command(gchar **prefix)
{
	GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);
	guint i;

	for (i = 0; prefix[i] != NULL; i++)
		g_ptr_array_add(argv, g_strdup(prefix[i]));
	return argv;
}

/* Runs @argv with @in as descriptor 3 and @second (or -1) as 4, which is
 * read only (the child may write no file). */
static void
run_tool(
	Verify             *v,
	GPtrArray          *argv,
	gint                in,
	gint                second,
	guint64             output_cap,
	AxisProbeRunResult *result
){
	AxisProbeRunSpec spec;

	g_ptr_array_add(argv, NULL);
	if (in >= 0)
		(void)lseek(in, 0, SEEK_SET);
	if (second >= 0)
		(void)lseek(second, 0, SEEK_SET);
	memset(&spec, 0, sizeof(spec));
	spec.argv = (const gchar *const *)argv->pdata;
	spec.fd = in;
	spec.output_fd = second >= 0 ? second : 0;
	spec.file_size_limit = 0;
	spec.timeout_ms = v->timeout_ms;
	spec.output_cap = (gsize)output_cap;
	spec.error_cap = ERROR_CAP;
	spec.cancellable = cancellable_of(v);
	spec.memory_limit = MEMORY_LIMIT;
	spec.working_directory = v->scratch;
	spec.cpu_seconds = v->timeout_ms / 1000 * 16 + 60;
	axis_probe_run(&spec, result);
}

static gchar *
tool_said(const AxisProbeRunResult *result)
{
	g_autofree gchar *raw = NULL;

	if (result->errors == NULL || result->errors->len == 0)
		return g_strdup(axis_probe_run_status_describe(result->status));
	raw = g_strndup((const gchar *)result->errors->data, result->errors->len);
	return axis_library_text_clean(raw, 300);
}

/* ── 1: the file asked for ──────────────────────────────────────── */

/* "a directory", "a symbolic link" ...: what a name that is not a plain
 * file is. */
static const gchar *
file_kind(mode_t mode)
{
	if (S_ISLNK(mode))
		return "a symbolic link";
	if (S_ISDIR(mode))
		return "a directory";
	if (S_ISFIFO(mode))
		return "a pipe";
	if (S_ISSOCK(mode))
		return "a socket";
	if (S_ISCHR(mode) || S_ISBLK(mode))
		return "a device";
	return "not a plain file";
}

static gboolean
check_identity(
	Verify  *v,
	gchar  **detail
){
	/* Named by its claim (0049), or by its attempt before. */
	g_autofree gchar *expected = axis_conversion_output_name(v->row->id, v->row->claims > 0 ? v->row->claims :
		v->row->attempt);
	struct stat st;
	gint flags;

	if (v->row->output_name == NULL || strcmp(v->row->output_name, expected) != 0 ||
	    !axis_convert_output_name_valid(v->row->output_name)) {
		/* The record's name (the one given at the claim), cleaned and cut
		 * short before it is shown. */
		g_autofree gchar *recorded = v->row->output_name != NULL ? axis_library_text_clean(v->row->output_name, 80) :
			NULL;

		if (recorded != NULL)
			*detail = g_strdup_printf("The output's name on record is \"%s\", not \"%s\", the one chosen for the "
				"attempt that completed.", recorded, expected);
		else
			*detail = g_strdup_printf("No output name is on record; the one chosen for the attempt that completed is "
				"\"%s\".", expected);
		return FALSE;
	}
	if (fstatat(v->work, v->row->output_name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
		*detail = g_strdup_printf("The output is not in the work area: %s.", g_strerror(errno));
		return FALSE;
	}
	if (!S_ISREG(st.st_mode)) {
		*detail = g_strdup_printf("The output is not a plain file with one name: it is %s.", file_kind(st.st_mode));
		return FALSE;
	}
	if (st.st_nlink != 1) {
		*detail = g_strdup_printf("The output is not a plain file with one name: it has %" G_GUINT64_FORMAT
			" names (hard links).", (guint64)st.st_nlink);
		return FALSE;
	}
	if ((guint64)st.st_dev != axis_volume_guard_get_dev(v->guard)) {
		*detail = g_strdup_printf("The output is not on the volume's own filesystem: it is on device %" G_GUINT64_FORMAT
			", the volume on device %" G_GUINT64_FORMAT ".", (guint64)st.st_dev, axis_volume_guard_get_dev(v->guard));
		return FALSE;
	}
	/* A plain file with one name in the work area, under the attempt's
	 * name: whatever else is wrong with it, it is no other file's name, so
	 * a rejection removes it (by this inode). */
	v->out_st = st;
	if (st.st_uid != geteuid() && st.st_uid != v->src_st.st_uid) {
		if (v->src_fd >= 0)
			*detail = g_strdup_printf("The output is owned by user %u, neither the daemon's user (%u) nor the "
				"original's owner (%u).", (guint)st.st_uid, (guint)geteuid(), (guint)v->src_st.st_uid);
		else
			*detail = g_strdup_printf("The output is owned by user %u, not the daemon's user (%u), and the original "
				"cannot be opened to see whose it is.", (guint)st.st_uid, (guint)geteuid());
		return FALSE;
	}
	if (st.st_size <= 0 || (gint64)st.st_size != v->row->output_reported) {
		*detail = g_strdup_printf("The output has %" G_GINT64_FORMAT " bytes; the worker reported %" G_GINT64_FORMAT ".",
			(gint64)st.st_size, v->row->output_reported);
		return FALSE;
	}
	v->out_fd = openat(v->work, v->row->output_name, O_RDONLY | O_NOFOLLOW | O_NOCTTY | O_CLOEXEC | O_NONBLOCK);
	{
		struct stat opened;

		if (v->out_fd < 0) {
			*detail = g_strdup_printf("The output could not be opened: %s.", g_strerror(errno));
			return FALSE;
		}
		if (fstat(v->out_fd, &opened) != 0) {
			*detail = g_strdup_printf("The output could not be looked at once opened: %s.", g_strerror(errno));
			return FALSE;
		}
		if (opened.st_ino != st.st_ino) {
			*detail = g_strdup_printf("The output opened is not the file that was looked at: inode %" G_GUINT64_FORMAT
				", where it was %" G_GUINT64_FORMAT ".", (guint64)opened.st_ino, (guint64)st.st_ino);
			return FALSE;
		}
		v->out_st = opened;
	}
	flags = fcntl(v->out_fd, F_GETFL);
	if (flags >= 0)
		(void)fcntl(v->out_fd, F_SETFL, flags & ~O_NONBLOCK);
	{
		g_autofree gchar *size = axis_size_format((guint64)st.st_size);

		*detail = g_strdup_printf("%s, a plain file in the work area, %s.", v->row->output_name, size);
	}
	return TRUE;
}

/* ── 2: the original unchanged ──────────────────────────────────── */

static gboolean
open_source(
	Verify  *v,
	gchar  **detail
){
	AxisStorageOpenStatus status;
	gint saved = 0;

	status = axis_storage_open_file_beneath(axis_volume_guard_get_fd(v->guard), v->row->root_path, v->row->rel_path,
		&v->src_fd, &v->src_st, &saved);
	v->src_status = status;
	if (status != AXIS_STORAGE_OPEN_OK) {
		*detail = g_strdup_printf("The original cannot be opened: %s.", axis_storage_open_status_describe(status));
		return FALSE;
	}
	return TRUE;
}

static gboolean
check_source(
	Verify  *v,
	gchar  **detail
){
	const AxisConversionRow *r = v->row;
	const struct stat *st = &v->src_st;
	GString *changed;

	if (v->src_fd < 0) {
		*detail = g_strdup_printf("The original cannot be opened: %s.",
			axis_storage_open_status_describe(v->src_status));
		return FALSE;
	}
	/* Each figure that is not what it was at the request, both ways. */
	changed = g_string_new(NULL);
	if ((gint64)st->st_size != r->src_size)
		g_string_append_printf(changed, "%sits size is %" G_GINT64_FORMAT " bytes, was %" G_GINT64_FORMAT,
			changed->len > 0 ? "; " : "", (gint64)st->st_size, r->src_size);
	if ((gint64)st->st_mtim.tv_sec != r->src_mtime_sec || (gint)st->st_mtim.tv_nsec != r->src_mtime_nsec)
		g_string_append_printf(changed, "%sits modification time is %" G_GINT64_FORMAT ".%09d, was %" G_GINT64_FORMAT
			".%09d", changed->len > 0 ? "; " : "", (gint64)st->st_mtim.tv_sec, (gint)st->st_mtim.tv_nsec,
			r->src_mtime_sec, r->src_mtime_nsec);
	if ((gint64)st->st_ctim.tv_sec != r->src_ctime_sec || (gint)st->st_ctim.tv_nsec != r->src_ctime_nsec)
		g_string_append_printf(changed, "%sits change time is %" G_GINT64_FORMAT ".%09d, was %" G_GINT64_FORMAT ".%09d",
			changed->len > 0 ? "; " : "", (gint64)st->st_ctim.tv_sec, (gint)st->st_ctim.tv_nsec, r->src_ctime_sec,
			r->src_ctime_nsec);
	if ((gint64)st->st_ino != r->src_inode)
		g_string_append_printf(changed, "%sits inode is %" G_GINT64_FORMAT ", was %" G_GINT64_FORMAT,
			changed->len > 0 ? "; " : "", (gint64)st->st_ino, r->src_inode);
	if (changed->len > 0) {
		*detail = g_strdup_printf("The original changed since the request: %s.", changed->str);
		g_string_free(changed, TRUE);
		return FALSE;
	}
	/* Each reason it is pinned. */
	if (st->st_nlink != 1)
		g_string_append_printf(changed, "it has %" G_GUINT64_FORMAT " names (hard links)", (guint64)st->st_nlink);
	if (v->facts.file == NULL)
		g_string_append_printf(changed, "%sit is no longer in the index", changed->len > 0 ? "; " : "");
	else if (v->facts.file->index.pinned)
		g_string_append_printf(changed, "%sthe index marks it pinned", changed->len > 0 ? "; " : "");
	if (v->facts.pinned_downloads)
		g_string_append_printf(changed, "%sit is under a downloads root", changed->len > 0 ? "; " : "");
	if (changed->len > 0) {
		*detail = g_strdup_printf("The original is pinned now: %s.", changed->str);
		g_string_free(changed, TRUE);
		return FALSE;
	}
	g_string_free(changed, TRUE);
	*detail = g_strdup("Its size, times to the nanosecond, inode and single link are those of the request.");
	return TRUE;
}

/* ── 3 and 4: the plan, the streams, the picture ────────────────── */

static gchar *
args_text(gchar **args)
{
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonGenerator) generator = json_generator_new();
	g_autoptr(JsonNode) root = NULL;
	guint i;

	json_builder_begin_array(builder);
	for (i = 0; args[i] != NULL; i++)
		json_builder_add_string_value(builder, args[i]);
	json_builder_end_array(builder);
	root = json_builder_get_root(builder);
	json_generator_set_root(generator, root);
	return json_generator_to_data(generator, NULL);
}

/* Where the argument vector @now differs from the one the worker was
 * given (@given, the JSON array the record keeps): the first argument
 * that is not the same, or the lengths. */
static gchar *
args_difference(
	gchar       **now,
	const gchar  *given
){
	g_autoptr(JsonParser) parser = json_parser_new();
	JsonNode *root;
	JsonArray *then;
	guint n_now = g_strv_length(now);
	guint n_then;
	guint i;

	if (given == NULL || !json_parser_load_from_data(parser, given, -1, NULL) ||
	    (root = json_parser_get_root(parser)) == NULL || !JSON_NODE_HOLDS_ARRAY(root))
		return g_strdup("the record of the arguments the worker was given cannot be read");
	then = json_node_get_array(root);
	n_then = json_array_get_length(then);
	for (i = 0; i < n_now && i < n_then; i++) {
		JsonNode *node = json_array_get_element(then, i);
		const gchar *was = JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING ?
			json_node_get_string(node) : NULL;

		if (g_strcmp0(now[i], was) != 0) {
			g_autofree gchar *a = axis_library_text_clean(now[i], 120);
			g_autofree gchar *b = was != NULL ? axis_library_text_clean(was, 120) : g_strdup("not text");
			g_autofree gchar *before = i > 0 ? axis_library_text_clean(now[i - 1], 60) : NULL;

			if (before != NULL)
				return g_strdup_printf("argument %u (after \"%s\") is \"%s\" now, it was \"%s\"", i + 1, before, a, b);
			return g_strdup_printf("argument %u is \"%s\" now, it was \"%s\"", i + 1, a, b);
		}
	}
	return g_strdup_printf("there are %u arguments now, there were %u", n_now, n_then);
}

/* Every difference check 3 found, joined; the first few only, and how many
 * more. */
static gchar *
mismatches_text(GPtrArray *mismatches)
{
	GString *out = g_string_new(NULL);
	guint shown = MIN(mismatches->len, 8);
	guint i;

	for (i = 0; i < shown; i++)
		g_string_append_printf(out, "%s%s", i > 0 ? "; " : "", (const gchar *)g_ptr_array_index(mismatches, i));
	if (mismatches->len > shown)
		g_string_append_printf(out, "; and %u more", mismatches->len - shown);
	if (out->len == 0)
		g_string_append(out, "it differs");
	return g_string_free(out, FALSE);
}

static gboolean
check_streams(
	Verify  *v,
	gchar  **detail,
	GError **error
){
	g_autoptr(GPtrArray) mismatches = g_ptr_array_new_with_free_func(g_free);
	g_autofree gchar *probe_error = NULL;
	g_autofree gchar *args = NULL;
	AxisConvertRefusal refusal = AXIS_CONVERT_OK;

	/* The plan made again from what was recorded when the work was handed
	 * out (the worker's encoders, the preset, the threads then): a setting
	 * or a worker's measurement changed since makes no difference.  When it
	 * still cannot be made as it was given (Axis itself changed, an upgrade
	 * between the encode and the checks), the converted file is not judged:
	 * that is neither the file's fault nor the revision's. */
	if (v->row->machine != NULL) {
		AxisDbConn *conn = axis_db_pool_acquire(v->self->pool, error);

		if (conn == NULL)
			return FALSE;
		v->plan = axis_conversion_plan_recorded(v->self, conn, &v->facts, v->row, &refusal, error);
		axis_db_pool_release(v->self->pool, conn);
	}
	if (v->plan == NULL) {
		if (error != NULL && *error != NULL)
			return FALSE;
		v->not_judged = g_strdup_printf("the plan cannot be made again as the worker was given it: %s",
			axis_convert_refusal_describe(refusal));
		*detail = g_strdup_printf("Not judged: %s.", v->not_judged);
		return FALSE;
	}
	args = args_text(v->plan->args);
	if (g_strcmp0(args, v->row->args) != 0) {
		g_autofree gchar *where = args_difference(v->plan->args, v->row->args);

		v->not_judged = g_strdup_printf("the plan made again from the conversion's records is not the one the worker was "
			"given (Axis changed since, an upgrade say): %s", where);
		*detail = g_strdup_printf("Not judged: %s.", v->not_judged);
		return FALSE;
	}
	if (!axis_probe_describe_fd((const gchar *const *)v->ffprobe, v->out_fd, v->timeout_ms, cancellable_of(v),
	                            &v->output, &probe_error) || v->output == NULL || g_strcmp0(v->output->status, "ok") != 0) {
		*detail = g_strdup_printf("ffprobe cannot read the output: %s", probe_error != NULL ? probe_error :
			v->output != NULL && v->output->error != NULL ? v->output->error : "it says nothing");
		return FALSE;
	}
	if (!axis_convert_plan_check_output(v->plan, v->output, mismatches)) {
		g_autofree gchar *all = mismatches_text(mismatches);

		*detail = g_strdup_printf("The output is not what the plan says: %s.", all);
		return FALSE;
	}
	*detail = g_strdup_printf("%u streams in the planned order, with the planned codecs, flags, languages and "
		"titles; %d chapters.", v->plan->streams->len, v->output->chapter_count);
	return TRUE;
}

static gboolean
check_picture(
	Verify  *v,
	gchar  **detail
){
	const AxisConvertStream *want;
	const AxisMediaStream *got;
	AxisCapabilityTool tool;
	g_autofree gchar *why = NULL;

	if (v->plan == NULL || v->output == NULL || v->plan->streams->len == 0) {
		*detail = g_strdup("There is no picture to compare (the streams could not be read).");
		return FALSE;
	}
	/* Size, depth, colour by what it means (an unstated range is limited),
	 * the HDR class and HDR10's values: by the same rules as check 3
	 * (axis-convert-check.c), with each difference named. */
	if (!axis_convert_plan_check_picture(v->plan, v->output, &why)) {
		*detail = g_steal_pointer(&why);
		return FALSE;
	}
	want = g_ptr_array_index(v->plan->streams, 0);
	got = g_ptr_array_index(v->output->streams, want->position);
	memset(&tool, 0, sizeof(tool));
	tool.ffmpeg = (const gchar *const *)v->ffmpeg;
	tool.ffprobe = (const gchar *const *)v->ffprobe;
	tool.scratch = v->scratch;
	tool.timeout_ms = v->timeout_ms;
	tool.cancellable = cancellable_of(v);
	why = axis_convert_check_bitstream(&tool, v->plan, v->out_fd);
	if (why != NULL) {
		*detail = g_strdup_printf("HDR10: %s", why);
		return FALSE;
	}
	/* What the output has, which is what the plan says it must. */
	*detail = g_strdup_printf("%dx%d, %d bits; colour as planned: primaries %s, transfer %s, matrix %s, range %s; "
		"%s%s.", got->width, got->height, got->bit_depth,
		axis_convert_colour_said("primaries", got->color_primaries),
		axis_convert_colour_said("transfer", got->color_transfer),
		axis_convert_colour_said("matrix", got->color_space), axis_convert_colour_said("range", got->color_range),
		got->hdr_format == want->hdr_format ? axis_hdr_format_label(got->hdr_format) :
		"SDR as planned (the probe does not classify a picture of more than 8 bits that states no transfer "
		"function)", want->hdr10.mastering || want->hdr10.light ? ", HDR10 values exact in the bitstream" : "");
	return TRUE;
}

/* ── The copied tracks' hashes (measured by checks 5 and 7) ─────── */

/* Whether the plan copies @s as packets (an attachment is held in the
 * container's header, not in packets: compared by its data, below). */
static gboolean
copied_packets(const AxisConvertStream *s)
{
	return s->action == AXIS_CONVERT_COPY && s->kind != AXIS_STREAM_ATTACHMENT;
}

static gboolean
copied_attachment(const AxisConvertStream *s)
{
	return s->action == AXIS_CONVERT_COPY && s->kind == AXIS_STREAM_ATTACHMENT;
}

static guint
count_copied_packets(const AxisConvertPlan *plan)
{
	guint n = 0;
	guint i;

	for (i = 0; plan != NULL && i < plan->streams->len; i++)
		n += copied_packets(g_ptr_array_index(plan->streams, i)) ? 1 : 0;
	return n;
}

static void
tools_of(
	Verify          *v,
	AxisVerifyTools *tools
){
	memset(tools, 0, sizeof(*tools));
	tools->ffmpeg = (const gchar *const *)v->ffmpeg;
	tools->ffprobe = (const gchar *const *)v->ffprobe;
	tools->scratch = v->scratch;
	tools->threads = v->threads;
	tools->cancellable = cancellable_of(v);
}

/* ── 5: whole ───────────────────────────────────────────────────── */

static gboolean
check_whole(
	Verify  *v,
	gchar  **detail
){
	if (v->checks == NULL) {
		*detail = g_strdup("A complete decode of the picture and the sound did not end cleanly: there is no plan.");
		return FALSE;
	}
	if (v->mode == AXIS_CONVERSION_CHECK_ALL) {
		AxisVerifyTools tools;

		tools_of(v, &tools);
		/* The same reading of the file hashes its copied tracks for check
		 * 7: the converted file is read once for both. */
		axis_verify_measure_decode(&tools, v->checks, v->out_fd, v->measured);
	}
	return axis_verify_judge_whole(evidence_of(v), detail);
}

/* ── 6: time ────────────────────────────────────────────────────── */

static const AxisMediaStream *
source_stream(
	const AxisMediaInfo *info,
	gint                 index
){
	guint i;

	for (i = 0; info != NULL && i < info->streams->len; i++) {
		const AxisMediaStream *s = g_ptr_array_index(info->streams, i);

		if (s->index == index)
			return s;
	}
	return NULL;
}

static gboolean
within(
	gint64 a,
	gint64 b
){
	gint64 tolerance = MAX((gint64)500, MAX(a, b) / 1000);

	return ABS(a - b) <= tolerance;
}

static gboolean
check_time(
	Verify  *v,
	gchar  **detail
){
	const AxisMediaInfo *source = v->facts.full;
	const AxisConvertStream *picture;
	g_autofree gchar *why = NULL;
	guint64 out_packets = 0;
	guint i;

	if (source == NULL || v->output == NULL || v->plan == NULL || v->checks == NULL) {
		*detail = g_strdup("There is nothing to compare the durations with.");
		return FALSE;
	}
	for (i = 0; i < v->plan->streams->len; i++) {
		const AxisConvertStream *want = g_ptr_array_index(v->plan->streams, i);
		const AxisMediaStream *in = want->source >= 0 ? source_stream(source, want->source) : NULL;
		const AxisMediaStream *out = want->position < v->output->streams->len ?
			g_ptr_array_index(v->output->streams, want->position) : NULL;
		gint64 a;
		gint64 b;

		if (want->source < 0 || want->kind == AXIS_STREAM_ATTACHMENT || want->attached_pic ||
		    want->kind == AXIS_STREAM_SUBTITLE)
			continue;
		if (out == NULL) {
			*detail = g_strdup_printf("The output has no stream %u, which the plan makes from stream %d of the "
				"original.", want->position, want->source);
			return FALSE;
		}
		if (in == NULL) {
			*detail = g_strdup_printf("Output stream %u is made from stream %d of the original, which the original's "
				"record does not have.", want->position, want->source);
			return FALSE;
		}
		a = in->duration_ms > 0 ? in->duration_ms : source->duration_ms;
		b = out->duration_ms > 0 ? out->duration_ms : v->output->duration_ms;
		if (a <= 0 || b <= 0) {
			*detail = g_strdup_printf("The duration of output stream %u or of its source is not known.", want->position);
			return FALSE;
		}
		if (!within(a, b)) {
			*detail = g_strdup_printf("Output stream %u lasts %" G_GINT64_FORMAT " ms; its source %" G_GINT64_FORMAT " ms.",
				want->position, b, a);
			return FALSE;
		}
	}
	picture = g_ptr_array_index(v->plan->streams, 0);
	if (v->mode == AXIS_CONVERSION_CHECK_ALL) {
		AxisVerifyTools tools;

		tools_of(v, &tools);
		axis_verify_measure_packets(&tools, v->checks, v->src_fd, v->out_fd, v->measured);
	}
	if (!axis_verify_judge_packets(evidence_of(v), &out_packets, &why)) {
		*detail = g_steal_pointer(&why);
		return FALSE;
	}
	v->picture_bytes = evidence_of(v)->output_packets.bytes;
	{
		const AxisMediaStream *out = picture->position < v->output->streams->len ?
			g_ptr_array_index(v->output->streams, picture->position) : NULL;

		v->picture_seconds = (gdouble)(out != NULL && out->duration_ms > 0 ? out->duration_ms :
			v->output->duration_ms) / 1000.0;
	}
	*detail = g_strdup_printf("Every stream's length within tolerance of its source's; %" G_GUINT64_FORMAT " picture "
		"packets in both.", out_packets);
	return TRUE;
}

/* ── 7: the copied tracks are the original's ────────────────────── */

/* The SHA-256 of each stream's codec data (an attachment's file is held
 * there), by stream index, read from the header only; %NULL when ffprobe
 * cannot read it. */
static GHashTable *
extradata_hashes(
	Verify  *v,
	gint     fd,
	gchar  **why
){
	g_autoptr(GPtrArray) argv = command(v->ffprobe);
	g_autoptr(GHashTable) hashes = g_hash_table_new_full(g_int_hash, g_int_equal, g_free, g_free);
	g_auto(GStrv) lines = NULL;
	g_autofree gchar *text = NULL;
	AxisProbeRunResult result;
	guint i;

	g_ptr_array_add(argv, g_strdup("-v"));
	g_ptr_array_add(argv, g_strdup("error"));
	g_ptr_array_add(argv, g_strdup("-hide_banner"));
	g_ptr_array_add(argv, g_strdup("-protocol_whitelist"));
	g_ptr_array_add(argv, g_strdup("fd"));
	g_ptr_array_add(argv, g_strdup("-format_whitelist"));
	g_ptr_array_add(argv, g_strdup(axis_probe_format_whitelist()));
	g_ptr_array_add(argv, g_strdup("-fd"));
	g_ptr_array_add(argv, g_strdup("3"));
	g_ptr_array_add(argv, g_strdup("-show_data_hash"));
	g_ptr_array_add(argv, g_strdup("sha256"));
	g_ptr_array_add(argv, g_strdup("-show_entries"));
	g_ptr_array_add(argv, g_strdup("stream=index,extradata_hash"));
	g_ptr_array_add(argv, g_strdup("-of"));
	g_ptr_array_add(argv, g_strdup("csv=p=0"));
	g_ptr_array_add(argv, g_strdup("-i"));
	g_ptr_array_add(argv, g_strdup("fd:"));
	run_tool(v, argv, fd, -1, 1024 * 1024, &result);
	if (result.status != AXIS_PROBE_RUN_OK) {
		*why = tool_said(&result);
		axis_probe_run_result_clear(&result);
		return NULL;
	}
	text = g_strndup((const gchar *)result.output->data, result.output->len);
	axis_probe_run_result_clear(&result);
	lines = g_strsplit(text, "\n", -1);
	for (i = 0; lines[i] != NULL; i++) {
		const gchar *comma;
		gchar *end = NULL;
		guint64 index;
		gint *key;

		g_strstrip(lines[i]);
		if (lines[i][0] == '\0')
			continue;
		index = g_ascii_strtoull(lines[i], &end, 10);
		comma = strchr(lines[i], ',');
		if (end == lines[i] || index > G_MAXINT || (comma != end && *end != '\0')) {
			*why = g_strdup("ffprobe's stream list is not one it writes");
			return NULL;
		}
		/* A stream without codec data is its index alone. */
		if (comma == NULL || comma[1] == '\0')
			continue;
		key = g_new(gint, 1);
		*key = (gint)index;
		g_hash_table_replace(hashes, key, g_strdup(comma + 1));
	}
	return g_steal_pointer(&hashes);
}

/*
 * Every track the plan copies (sound, subtitles, cover pictures, data) must
 * be the original's track bit for bit: the SHA-256 of its packet payloads,
 * in order, equal to the original's (the converted file's hashes were made
 * by check 5's reading of it; the original is read once more, demuxed only,
 * nothing decoded: here, or on the worker that measured 5).  An attachment
 * (a font) must have the original's data exactly: read here from both
 * headers.  Tracks the plan makes anew (a stereo AAC track, a text subtitle
 * in MP4's own format) have no original to be equal to: they are compared
 * by the streams (3), the length (6) and a clean decode (5) only, and this
 * says so.
 */
static gboolean
check_copied(
	Verify  *v,
	gchar  **detail
){
	g_autoptr(GHashTable) src_data = NULL;
	g_autoptr(GHashTable) out_data = NULL;
	guint copied = count_copied_packets(v->plan);
	guint sound = 0;
	guint subtitles = 0;
	guint other = 0;
	guint attachments = 0;
	guint stereo = 0;
	guint texts = 0;
	guint i;
	GString *said;

	if (v->plan == NULL) {
		*detail = g_strdup("There is no plan to compare with.");
		return FALSE;
	}
	for (i = 0; i < v->plan->streams->len; i++) {
		const AxisConvertStream *s = g_ptr_array_index(v->plan->streams, i);

		if (copied_attachment(s))
			attachments++;
		else if (copied_packets(s) && s->kind == AXIS_STREAM_AUDIO)
			sound++;
		else if (copied_packets(s) && s->kind == AXIS_STREAM_SUBTITLE)
			subtitles++;
		else if (copied_packets(s))
			other++;
		else if (s->action == AXIS_CONVERT_STEREO)
			stereo++;
		else if (s->action == AXIS_CONVERT_TO_TEXT)
			texts++;
	}

	if (copied > 0) {
		g_autofree gchar *why = NULL;

		if (v->checks == NULL || v->checks->n_tracks != copied) {
			*detail = g_strdup("The converted file's copied tracks could not be read for the comparison.");
			return FALSE;
		}
		/* The original is read only when the converted file's tracks were. */
		if (v->mode == AXIS_CONVERSION_CHECK_ALL && v->measured->output_hashes != NULL) {
			AxisVerifyTools tools;

			tools_of(v, &tools);
			axis_verify_measure_tracks(&tools, v->checks, v->src_fd, v->measured);
		}
		if (!axis_verify_judge_tracks(evidence_of(v), v->plan, &why)) {
			*detail = g_steal_pointer(&why);
			return FALSE;
		}
	}

	if (attachments > 0) {
		g_autofree gchar *why = NULL;

		src_data = extradata_hashes(v, v->src_fd, &why);
		if (src_data != NULL)
			out_data = extradata_hashes(v, v->out_fd, &why);
		if (src_data == NULL || out_data == NULL) {
			*detail = g_strdup_printf("The attachments could not be read for the comparison: %s", why);
			return FALSE;
		}
		for (i = 0; i < v->plan->streams->len; i++) {
			const AxisConvertStream *s = g_ptr_array_index(v->plan->streams, i);
			gint source = s->source;
			gint position = (gint)s->position;
			const gchar *a;
			const gchar *b;

			if (!copied_attachment(s))
				continue;
			a = g_hash_table_lookup(src_data, &source);
			b = g_hash_table_lookup(out_data, &position);
			if (b == NULL) {
				*detail = g_strdup_printf("The attachment that is stream %u of the converted file (stream %d of the "
					"original) is missing from the converted file: it has no data there.", s->position, s->source);
				return FALSE;
			}
			if (a == NULL) {
				*detail = g_strdup_printf("The attachment that is stream %u of the converted file (stream %d of the "
					"original) has no data in the original to compare with.", s->position, s->source);
				return FALSE;
			}
			if (g_ascii_strcasecmp(a, b) != 0) {
				*detail = g_strdup_printf("The attachment that is stream %u of the converted file (stream %d of the "
					"original) is not the original's: its data differs (SHA-256 %.12s... in the converted file, "
					"%.12s... in the original).", s->position, s->source, b, a);
				return FALSE;
			}
		}
	}

	said = g_string_new(NULL);
	if (copied + attachments == 0) {
		g_string_append(said, "No track was copied.");
	} else {
		g_string_append(said, "Bit for bit the original's (every packet's data, in order): ");
		if (sound > 0)
			g_string_append_printf(said, "%u sound track%s", sound, sound == 1 ? "" : "s");
		if (subtitles > 0)
			g_string_append_printf(said, "%s%u subtitle track%s", sound > 0 ? ", " : "", subtitles,
				subtitles == 1 ? "" : "s");
		if (other > 0)
			g_string_append_printf(said, "%s%u other stream%s", sound + subtitles > 0 ? ", " : "", other,
				other == 1 ? "" : "s");
		if (attachments > 0)
			g_string_append_printf(said, "%s%u attachment%s", copied > 0 ? ", " : "", attachments,
				attachments == 1 ? "" : "s");
		g_string_append_c(said, '.');
	}
	if (stereo + texts > 0) {
		g_string_append(said, " Made anew, so compared by the streams, the length and a clean decode only, not bit "
			"for bit: ");
		if (stereo > 0)
			g_string_append_printf(said, "%u stereo sound track%s", stereo, stereo == 1 ? "" : "s");
		if (texts > 0)
			g_string_append_printf(said, "%s%u subtitle track%s in the container's own text format",
				stereo > 0 ? ", " : "", texts, texts == 1 ? "" : "s");
		g_string_append_c(said, '.');
	}
	*detail = g_string_free(said, FALSE);
	return TRUE;
}

/* ── 8: the same picture ────────────────────────────────────────── */

/* At each point of the plan (none when the movie's length is not known), a
 * window of both files compared with SSIM; this daemon, measuring itself,
 * compares no further point after one that fails. */
static gboolean
check_same_picture(
	Verify  *v,
	gchar  **detail
){
	gdouble floor_value = axis_conversion_setting_decimal(v->self, AXIS_CONVERSION_SETTING_SIMILARITY, 0.85);

	if (v->checks == NULL) {
		*detail = g_strdup("The movie's length is not known: no points to compare at.");
		return FALSE;
	}
	if (v->mode == AXIS_CONVERSION_CHECK_ALL) {
		AxisVerifyTools tools;
		guint k;

		tools_of(v, &tools);
		for (k = 0; k < v->checks->n_points; k++) {
			axis_verify_measure_point(&tools, v->checks, k, v->src_fd, v->out_fd, v->measured);
			if (axis_verify_point_fails(v->measured, k, floor_value))
				break;
		}
	}
	return axis_verify_judge_picture(evidence_of(v), v->checks->n_points, floor_value, detail);
}

/* ── 9: worth it ────────────────────────────────────────────────── */

static gboolean
check_worth_it(
	Verify  *v,
	gchar  **detail
){
	const AxisProfileDefinition *def = v->facts.revision != NULL ? &v->facts.revision->def : NULL;
	gint64 before = (gint64)v->src_st.st_size;
	gint64 after = (gint64)v->out_st.st_size;
	gint64 needed;
	g_autofree gchar *a = NULL;
	g_autofree gchar *b = NULL;

	if (def == NULL) {
		*detail = g_strdup("The profile's minimum saving cannot be read.");
		return FALSE;
	}
	needed = MAX(before / 100 * def->min_saving_percent, def->min_saving_bytes);
	a = axis_size_format((guint64)MAX(after, 0));
	b = axis_size_format((guint64)MAX(before, 0));
	if (before - after < needed || after >= before) {
		g_autofree gchar *need = axis_size_format((guint64)MAX(needed, 0));

		if (after >= before || before <= 0)
			*detail = g_strdup_printf("%s against %s: no smaller at all, so not smaller by the profile's minimum saving "
				"(%d%%, here %s).", a, b, def->min_saving_percent, need);
		else {
			g_autofree gchar *saved = axis_size_format((guint64)(before - after));

			*detail = g_strdup_printf("%s against %s: %s (%.1f%%) smaller, not smaller by the profile's minimum saving "
				"(%d%%, here %s).", a, b, saved, 100.0 * (gdouble)(before - after) / (gdouble)before,
				def->min_saving_percent, need);
		}
		return FALSE;
	}
	*detail = g_strdup_printf("%s against %s: %.1f%% smaller (at least %d%% needed).", a, b,
		100.0 * (gdouble)(before - after) / (gdouble)before, def->min_saving_percent);
	return TRUE;
}

/* ── 10: the gates ──────────────────────────────────────────────── */

static gboolean
check_gates(
	Verify  *v,
	gchar  **detail,
	GError **error
){
	g_autoptr(GPtrArray) gates = g_ptr_array_new();
	AxisConversionInputs inputs;
	AxisDbConn *conn;
	gboolean ok;
	guint i;

	/* Everything read again now, just before placement. */
	if (!load_facts(v, error))
		return FALSE;
	axis_conversion_inputs_load(v->self, &v->facts, &inputs);
	conn = axis_db_pool_acquire(v->self->pool, error);
	if (conn == NULL) {
		axis_conversion_inputs_clear(&inputs);
		return FALSE;
	}
	ok = axis_conversion_gates(v->self, conn, &v->facts, &inputs, v->row->outcome, v->guard, FALSE, v->row, NULL,
		gates, error);
	axis_db_pool_release(v->self->pool, conn);
	axis_conversion_inputs_clear(&inputs);
	if (!ok)
		return FALSE;
	for (i = 0; i < gates->len; i++) {
		AxisConversionGate *gate = g_ptr_array_index(gates, i);

		if (*detail == NULL && gate->applies && !gate->passed)
			*detail = g_strdup_printf("%s: %s", gate->label, gate->detail != NULL ? gate->detail : "it does not hold.");
	}
	for (i = 0; i < gates->len; i++) {
		AxisConversionGate *gate = g_ptr_array_index(gates, i);

		g_free(gate->detail);
		g_free(gate);
	}
	if (*detail != NULL)
		return FALSE;
	*detail = g_strdup("Every gate of the request holds now.");
	return TRUE;
}

/* ── The calibration sample ─────────────────────────────────────── */

/* A verified conversion is a real result for the estimator: the source's
 * facts, the revision, the family that encoded, the measured bitrate of the
 * picture (its packets' bytes over its length).  One it would not accept is
 * noted, never a reason to fail. */
static void
record_sample(Verify *v)
{
	g_autoptr(GError) local = NULL;
	AxisEstimateSample sample;
	AxisEstimateShape shape;

	if (v->facts.file == NULL || v->facts.file->info == NULL || !v->facts.has_target || v->plan == NULL ||
	    v->picture_seconds <= 0 || v->picture_bytes == 0)
		return;
	axis_estimate_shape_init(&shape, v->facts.file->info);
	if (shape.video == NULL)
		return;
	memset(&sample, 0, sizeof(sample));
	sample.target = v->facts.target.id;
	sample.revision_id = v->facts.target.revision_id;
	sample.has_output_facts = TRUE;
	sample.out_bit_depth = v->plan->bit_depth;
	sample.codec = v->plan->encoder->codec;
	sample.family = v->plan->encoder->family;
	sample.quality = v->plan->level;
	sample.part = "video";
	sample.source_codec = shape.video->codec;
	sample.width = shape.video->width;
	sample.height = shape.video->height;
	sample.frame_rate = shape.fps;
	sample.source_bitrate = shape.video_bitrate;
	sample.seconds = v->picture_seconds;
	sample.output_bitrate = (gint64)((gdouble)v->picture_bytes * 8.0 / v->picture_seconds);
	if (!axis_estimator_record_sample(v->self->estimator, NULL, &sample, "converted", 0, v->row->file_id, &local))
		g_message("conversions: conversion %" G_GINT64_FORMAT " is not a calibration sample: %s", v->row->id,
			local->message);
}

/* ── A worker's measurements bound to the files ─────────────────── */

typedef struct
{
	gint     fd;
	gint64   size;
	gint     stop;	/* atomic */
	gchar   *hex;
	gint     err;
	gboolean ok;
} Whole;

/* The whole file's SHA-256, read on a thread of its own at the lowest CPU
 * priority and the idle I/O class (a thread's own, on Linux), as the tools
 * run. */
static gpointer
whole_thread(gpointer data)
{
	Whole *w = data;
	pid_t tid = (pid_t)syscall(SYS_gettid);

	(void)setpriority(PRIO_PROCESS, (id_t)tid, 19);
	(void)syscall(SYS_ioprio_set, IOPRIO_WHO_PROCESS, (int)tid, IOPRIO_CLASS_IDLE_VALUE);
	w->ok = axis_file_whole_digest(w->fd, w->size, &w->stop, &w->hex, &w->err);
	return NULL;
}

static void
whole_cancelled(
	GCancellable *cancellable,
	gpointer      data
){
	g_atomic_int_set((gint *)data, 1);
}

/* FALSE with @error when the file cannot be read now (nothing concluded). */
static gboolean
whole_digest(
	Verify  *v,
	gchar  **hex,
	GError **error
){
	GCancellable *cancellable = cancellable_of(v);
	GThread *thread;
	gulong handler = 0;
	Whole w;

	*hex = NULL;
	memset(&w, 0, sizeof(w));
	w.fd = v->out_fd;
	w.size = (gint64)v->out_st.st_size;
	if (cancellable != NULL)
		handler = g_cancellable_connect(cancellable, G_CALLBACK(whole_cancelled), &w.stop, NULL);
	thread = g_thread_new("axis-whole-digest", whole_thread, &w);
	(void)g_thread_join(thread);
	if (cancellable != NULL)
		g_cancellable_disconnect(cancellable, handler);
	if (!w.ok) {
		if (w.err == ECANCELED)
			g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "The checks were stopped.");
		else
			g_set_error(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_BUSY,
				"the converted file cannot be read whole now: %s", g_strerror(w.err != 0 ? w.err : EIO));
		return FALSE;
	}
	*hex = w.hex;
	return TRUE;
}

/*
 * Whether the worker's measurements are of these files (design.md of
 * verify-on-worker, "Binding the evidence to the files"): the converted
 * file's stamp now is the one it had when it was handed out, the plan made
 * now is the one the worker was given, the samples of both files the worker
 * read are the ones read here, and (convert.confirm_whole_file) every byte.
 * FALSE with *@why when they are not; FALSE with @error when a file cannot
 * be read now.
 */
static gboolean
bind_evidence(
	Verify  *v,
	gchar  **why,
	GError **error
){
	const AxisConversionRow *r = v->row;
	const AxisVerifyEvidence *e = v->given;
	const struct stat *st = &v->out_st;
	g_autofree gchar *plan = NULL;
	g_autofree gchar *sample = NULL;
	gint err = 0;

	*why = NULL;
	if (e == NULL || !r->handed || r->check_plan == NULL || r->check_source_sample == NULL) {
		*why = g_strdup("this server has no record of handing the checks out");
		return FALSE;
	}
	if ((gint64)st->st_ino != r->handed_inode || (gint64)st->st_size != r->handed_size ||
	    (gint64)st->st_mtim.tv_sec != r->handed_mtime_sec || (gint)st->st_mtim.tv_nsec != r->handed_mtime_nsec ||
	    (gint64)st->st_ctim.tv_sec != r->handed_ctime_sec || (gint)st->st_ctim.tv_nsec != r->handed_ctime_nsec) {
		*why = g_strdup_printf("the converted file changed after its checks were handed out (inode %" G_GINT64_FORMAT
			", %" G_GINT64_FORMAT " bytes, change time %" G_GINT64_FORMAT ".%09d now; inode %" G_GINT64_FORMAT ", %"
			G_GINT64_FORMAT " bytes, change time %" G_GINT64_FORMAT ".%09d then)", (gint64)st->st_ino,
			(gint64)st->st_size, (gint64)st->st_ctim.tv_sec, (gint)st->st_ctim.tv_nsec, r->handed_inode,
			r->handed_size, r->handed_ctime_sec, r->handed_ctime_nsec);
		return FALSE;
	}
	plan = v->checks != NULL ? axis_verify_plan_to_json(v->checks) : NULL;
	if (plan == NULL || strcmp(plan, r->check_plan) != 0) {
		*why = g_strdup("the plan of the checks made now is not the one the worker was given");
		return FALSE;
	}
	if (g_strcmp0(e->source_sample, r->check_source_sample) != 0) {
		*why = g_strdup_printf("the samples of the original the worker read (%.12s...) are not the ones this server "
			"read (%.12s...)", e->source_sample != NULL ? e->source_sample : "none", r->check_source_sample);
		return FALSE;
	}
	if (!axis_file_sample_digest(v->out_fd, (gint64)st->st_size, &sample, &err)) {
		g_set_error(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_BUSY,
			"the converted file cannot be read now: %s", g_strerror(err != 0 ? err : EIO));
		return FALSE;
	}
	if (g_strcmp0(e->output_sample, sample) != 0) {
		*why = g_strdup_printf("the samples of the converted file the worker read (%.12s...) are not the ones this "
			"server reads (%.12s...)", e->output_sample != NULL ? e->output_sample : "none", sample);
		return FALSE;
	}
	g_free(v->output_sample);
	v->output_sample = g_strdup(sample);
	if (axis_conversion_setting_boolean(v->self, AXIS_CONVERSION_SETTING_CONFIRM_WHOLE, TRUE)) {
		g_autofree gchar *whole = NULL;

		if (!whole_digest(v, &whole, error))
			return FALSE;
		if (g_strcmp0(e->output_digest, whole) != 0) {
			*why = g_strdup_printf("the converted file read whole here (SHA-256 %.12s...) is not the one the worker "
				"read (%.12s...)", whole, e->output_digest != NULL ? e->output_digest : "none");
			return FALSE;
		}
		g_free(v->output_digest);
		v->output_digest = g_steal_pointer(&whole);
	}
	return TRUE;
}

/* ── The order ──────────────────────────────────────────────────── */

static void
verify_clear(Verify *v)
{
	axis_conversion_facts_clear(&v->facts);
	g_strfreev(v->ffmpeg);
	g_strfreev(v->ffprobe);
	if (v->scratch != NULL) {
		(void)g_rmdir(v->scratch);
		g_free(v->scratch);
	}
	if (v->out_fd >= 0)
		close(v->out_fd);
	if (v->src_fd >= 0)
		close(v->src_fd);
	if (v->plan != NULL)
		axis_convert_plan_free(v->plan);
	if (v->output != NULL)
		axis_media_info_free(v->output);
	axis_verify_evidence_free(v->measured);
	axis_verify_plan_free(v->checks);
	g_free(v->not_judged);
	g_free(v->output_sample);
	g_free(v->output_digest);
}

void
axis_conversion_verify_clear(AxisConversionVerify *found)
{
	if (found == NULL)
		return;
	axis_verify_plan_free(found->plan);
	g_free(found->source_sample);
	g_free(found->unbound);
	g_free(found->not_judged);
	g_free(found->output_sample);
	g_free(found->output_digest);
	found->plan = NULL;
	found->source_sample = NULL;
	found->unbound = NULL;
	found->not_judged = NULL;
	found->output_sample = NULL;
	found->output_digest = NULL;
}

/*
 * AGAIN: whether the output in the work area is the file whose checks all
 * passed (the row's passed_* record): the same inode and size, the daemon's
 * samples of it the same, and its stamp either as it was then, as Axis left
 * it after settling it (the row's output_* record), or, measured on a worker
 * after a stop between settling and that record, in the one shape settling
 * gives (the original's modification time, a change time not earlier); a
 * whole-file digest recorded then is required again in that last case.
 * FALSE with *@why when it is not; FALSE with @error when it cannot be read.
 */
static gboolean
is_passed_file(
	Verify  *v,
	gchar  **why,
	GError **error
){
	const AxisConversionRow *r = v->row;
	const struct stat *st = &v->out_st;
	g_autofree gchar *sample = NULL;
	gboolean as_passed;
	gboolean as_settled;
	gboolean as_settling;
	gint err = 0;

	*why = NULL;
	if ((gint64)st->st_ino != r->passed_inode || (gint64)st->st_size != r->passed_size) {
		*why = g_strdup("it is not the same file (its inode or size)");
		return FALSE;
	}
	as_passed = (gint64)st->st_mtim.tv_sec == r->passed_mtime_sec && (gint)st->st_mtim.tv_nsec == r->passed_mtime_nsec &&
		(gint64)st->st_ctim.tv_sec == r->passed_ctime_sec && (gint)st->st_ctim.tv_nsec == r->passed_ctime_nsec;
	as_settled = r->output_inode == r->passed_inode && (gint64)st->st_mtim.tv_sec == r->output_mtime_sec &&
		(gint)st->st_mtim.tv_nsec == r->output_mtime_nsec && (gint64)st->st_ctim.tv_sec == r->output_ctime_sec &&
		(gint)st->st_ctim.tv_nsec == r->output_ctime_nsec;
	as_settling = g_strcmp0(r->checked_where, "worker") == 0 && (gint64)st->st_mtim.tv_sec == r->src_mtime_sec &&
		(gint)st->st_mtim.tv_nsec == r->src_mtime_nsec &&
		((gint64)st->st_ctim.tv_sec > r->passed_ctime_sec ||
		 ((gint64)st->st_ctim.tv_sec == r->passed_ctime_sec && (gint)st->st_ctim.tv_nsec >= r->passed_ctime_nsec));
	if (!as_passed && !as_settled && !as_settling) {
		*why = g_strdup("its times are neither those it had then nor those Axis gave it when it settled it");
		return FALSE;
	}
	if (!axis_file_sample_digest(v->out_fd, (gint64)st->st_size, &sample, &err)) {
		g_set_error(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_BUSY,
			"the converted file cannot be read now: %s", g_strerror(err != 0 ? err : EIO));
		return FALSE;
	}
	if (g_strcmp0(sample, r->passed_sample) != 0) {
		*why = g_strdup("samples of its content are not the ones read then");
		return FALSE;
	}
	if (!as_passed && !as_settled && r->passed_digest != NULL) {
		g_autofree gchar *whole = NULL;

		if (!whole_digest(v, &whole, error))
			return FALSE;
		if (g_strcmp0(whole, r->passed_digest) != 0) {
			*why = g_strdup("its whole content is not the one read then");
			return FALSE;
		}
	}
	return TRUE;
}

/* What 5 to 8 measure, from the plan made again by check 3. */
static void
make_checks(Verify *v)
{
	guint points;

	g_mutex_lock(&v->self->lock);
	points = v->self->tuning.similarity_points != 0 ? v->self->tuning.similarity_points : POINTS;
	g_mutex_unlock(&v->self->lock);
	axis_verify_plan_free(v->checks);
	v->checks = v->plan != NULL ? axis_verify_plan_make(v->plan, v->facts.full != NULL ?
		v->facts.full->duration_ms : 0, points, MAX(v->timeout_ms, (guint)AXIS_VERIFY_MIN_TIMEOUT_MS)) : NULL;
}

/* The checks from @from on recorded as not run, saying @why. */
static gboolean
not_run_from(
	Verify       *v,
	guint         from,
	const gchar  *why,
	GError      **error
){
	guint n;

	for (n = from; n <= AXIS_CONVERSION_N_CHECKS; n++) {
		if (!axis_conversion_record_check_pool(v->self->pool, v->row->id, n, "skipped", why, error))
			return FALSE;
	}
	return TRUE;
}

gboolean
axis_conversion_verify(
	AxisConversions            *self,
	AxisConversionRow          *row,
	AxisVolumeGuard            *guard,
	gint                        work,
	AxisJobContext             *context,
	AxisConversionCheckMode     mode,
	const AxisVerifyEvidence   *evidence,
	AxisConversionVerify       *found,
	guint                      *failed,
	GError                    **error
){
	Verify v;
	gboolean ok = FALSE;
	guint n;

	*failed = 0;
	memset(found, 0, sizeof(*found));
	memset(&v, 0, sizeof(v));
	v.self = self;
	v.row = row;
	v.guard = guard;
	v.work = work;
	v.context = context;
	v.out_fd = -1;
	v.src_fd = -1;
	v.mode = mode;
	v.given = evidence;
	v.measured = axis_verify_evidence_new();
	v.threads = (guint)CLAMP(axis_conversion_setting_integer(self, AXIS_CONVERSION_SETTING_CHECK_THREADS, 2), 1,
		AXIS_VERIFY_MAX_THREADS);
	v.ffmpeg = axis_conversion_ffmpeg(self);
	v.ffprobe = axis_conversion_ffprobe(self);
	if (mode == AXIS_CONVERSION_CHECK_JUDGE && evidence == NULL) {
		g_set_error_literal(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_FAILED,
			"there are no measurements to judge");
		goto out;
	}
	if (!load_facts(&v, error))
		goto out;
	v.timeout_ms = axis_conversion_tool_timeout(self, v.facts.file != NULL && v.facts.file->info != NULL ?
		v.facts.file->info->duration_ms : 0);
	{
		g_autofree gchar *parent = axis_estimator_scratch_parent(self->estimator, error);
		g_autofree gchar *template = NULL;

		if (parent == NULL)
			goto out;
		template = g_build_filename(parent, "convert-XXXXXX", NULL);
		if (g_mkdtemp_full(template, 0700) == NULL) {
			g_set_error(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_FAILED,
				"no scratch directory can be made for the checks: %s", g_strerror(errno));
			goto out;
		}
		v.scratch = g_steal_pointer(&template);
	}

	for (n = 1; n <= AXIS_CONVERSION_N_CHECKS; n++) {
		g_autofree gchar *detail = NULL;
		gboolean passed = FALSE;

		if (stopped(&v)) {
			g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "The checks were stopped.");
			goto out;
		}
		if (context != NULL)
			axis_job_context_set_progress(context, (gdouble)(n - 1) / AXIS_CONVERSION_N_CHECKS,
				axis_conversion_check_label(n));
		switch (n) {
		case 1:
			/* The original's owner is one the output may have. */
			if (!open_source(&v, &detail)) {
				g_clear_pointer(&detail, g_free);
				memset(&v.src_st, 0, sizeof(v.src_st));
				v.src_st.st_uid = (uid_t)-1;
			}
			passed = check_identity(&v, &detail);
			/* Every check passed already: only for the very same file are
			 * they not made again. */
			if (passed && mode == AXIS_CONVERSION_CHECK_AGAIN) {
				g_autofree gchar *why = NULL;

				if (!is_passed_file(&v, &why, error)) {
					if (why == NULL)
						goto out;
					g_message("conversions: conversion %" G_GINT64_FORMAT ": the converted file is not the one whose "
						"checks passed (%s): it is checked from the start", row->id, why);
					found->not_passed = TRUE;
					goto out;
				}
				{
					gchar *more = g_strdup_printf("%s Every check of it passed before; it is the same file, so checks 3 "
						"to 8 are not made again.", detail);

					g_free(detail);
					detail = more;
				}
			}
			break;
		case 2:
			passed = check_source(&v, &detail);
			break;
		case 3:
			if (mode == AXIS_CONVERSION_CHECK_AGAIN) {
				n = 8;
				continue;
			}
			passed = check_streams(&v, &detail, error);
			if (!passed && error != NULL && *error != NULL)
				goto out;
			/* The plan could not be made as the worker was given it: the
			 * converted file is not judged (no check of it was made). */
			if (!passed && v.not_judged != NULL) {
				if (!axis_conversion_record_check_pool(self->pool, row->id, 3, "skipped", detail, error) ||
				    !not_run_from(&v, 4, "Not run: the converted file was not judged.", error))
					goto out;
				found->not_judged = g_steal_pointer(&v.not_judged);
				found->output = v.out_st;
				found->source = v.src_st;
				goto out;
			}
			/* 4 runs with it, whatever 3 found. */
			{
				g_autofree gchar *picture = NULL;
				gboolean picture_ok = check_picture(&v, &picture);

				if (!record(&v, 3, passed, detail, error) || !record(&v, 4, picture_ok, picture, error))
					goto out;
				if (!passed || !picture_ok) {
					*failed = passed ? 4 : 3;
					goto rejected;
				}
				make_checks(&v);
				if (mode == AXIS_CONVERSION_CHECK_HAND_OUT) {
					gint err = 0;

					/* What the worker needs: the plan, the original's samples as
					 * this disk has them, the converted file's stamp now. */
					if (v.checks == NULL) {
						g_set_error_literal(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_FAILED,
							"the plan of the checks cannot be made");
						goto out;
					}
					if (!axis_file_sample_digest(v.src_fd, row->src_size, &found->source_sample, &err)) {
						g_set_error(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_BUSY,
							"the original cannot be read now: %s", g_strerror(err != 0 ? err : EIO));
						goto out;
					}
					found->plan = g_steal_pointer(&v.checks);
					found->output = v.out_st;
					found->source = v.src_st;
					ok = TRUE;
					goto out;
				}
				if (mode == AXIS_CONVERSION_CHECK_JUDGE) {
					g_autofree gchar *why = NULL;

					if (!bind_evidence(&v, &why, error)) {
						if (why == NULL)
							goto out;
						/* Not of these files: not used, nothing concluded of the
						 * file. */
						if (!not_run_from(&v, 5, "Not run: the measurements made on the worker are not of this "
						        "file.", error))
							goto out;
						found->unbound = g_steal_pointer(&why);
						found->output = v.out_st;
						found->source = v.src_st;
						goto out;
					}
				}
				n = 4;
				continue;
			}
		case 5:
			passed = check_whole(&v, &detail);
			break;
		case 6:
			passed = check_time(&v, &detail);
			break;
		case 7:
			passed = check_copied(&v, &detail);
			break;
		case 8:
			passed = check_same_picture(&v, &detail);
			break;
		case 9:
			passed = check_worth_it(&v, &detail);
			found->worth_it_failed = !passed;
			break;
		default:
			passed = check_gates(&v, &detail, error);
			if (!passed && error != NULL && *error != NULL)
				goto out;
			break;
		}
		if (stopped(&v)) {
			g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "The checks were stopped.");
			goto out;
		}
		if (!record(&v, n, passed, detail, error))
			goto out;
		if (!passed) {
			*failed = n;
			goto rejected;
		}
	}
	/* What a retry needs to know it is the same file (axis-conversion-
	 * tidy.c records it before anything changes the file). */
	if (mode != AXIS_CONVERSION_CHECK_AGAIN && v.output_sample == NULL) {
		gint err = 0;

		if (!axis_file_sample_digest(v.out_fd, (gint64)v.out_st.st_size, &v.output_sample, &err)) {
			g_set_error(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_BUSY,
				"the converted file cannot be read now: %s", g_strerror(err != 0 ? err : EIO));
			goto out;
		}
	}
	found->output = v.out_st;
	found->source = v.src_st;
	found->picture_bytes = v.picture_bytes;
	found->picture_seconds = v.picture_seconds;
	found->output_sample = g_steal_pointer(&v.output_sample);
	found->output_digest = g_steal_pointer(&v.output_digest);
	/* Recorded once, when the checks were made. */
	if (mode != AXIS_CONVERSION_CHECK_AGAIN)
		record_sample(&v);
	ok = TRUE;
	goto out;

rejected:
	/* 3 and 4 are both recorded already when either failed. */
	for (n = *failed == 3 || *failed == 4 ? 5 : *failed + 1; n <= AXIS_CONVERSION_N_CHECKS; n++) {
		if (!axis_conversion_record_check_pool(self->pool, row->id, n, "skipped", "Not run: an earlier check failed.",
		        error))
			break;
	}
	found->output = v.out_st;
	found->source = v.src_st;
	ok = FALSE;
out:
	verify_clear(&v);
	return ok;
}
